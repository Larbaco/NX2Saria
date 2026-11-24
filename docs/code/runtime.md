# runtime (top level)

The files directly under `runtime/`: the entry point and its modes, the boot and the loader, guest memory, the dispatch
of lifted code, host functions, the log and what it hides, the settings header, the session summary, and small helpers
the folders share. Where the area sits in the port: "3. Host" and "Runtime folders" in
[architecture.md](../architecture.md). The feature docs the notes point to: [logging.md](../logging.md),
[log-privacy.md](../log-privacy.md), [telemetry.md](../telemetry.md), [performance.md](../performance.md),
[tracker-api.md](../tracker-api.md).

In the notes "the game" is the title the build is for, by version (1.1.0, 1.1.1, 1.2.1, 1.4.3). `main+0x...` is an
address in the game's main module, `sdk+0x...` one in its SDK module. "Horizon" is the console's operating system and
"the SDK" the game's own copy of the `nn::` libraries, which runs as lifted code. A file whose first lines are a licence
notice keeps them in the code; the rows of `THIRD_PARTY_NOTICES.md` say what each notice covers.

## `runtime/main.c`

The shared game entry point: the standalone build names it `nx2saria.exe`; the launcher build retains `ocarina.exe`. It
reserves the guest address space, parses the command line and runs one mode. Also here: the runtime services the
generated code calls besides `rt_dispatch()` and `rt_svc()` (`rt_brk()`, `rt_udf()`, `rt_ticks()`, the function profiler
hook), logging, SHA-256, and the crash reporter (a vectored exception handler that names the lifted function, the guest
registers and the guest frame-pointer chain). The file starts with a UTF-8 byte order mark, which the comment removal
left in place.

The command line is `<game executable> [--exefs DIR] [--romfs DIR] [--profile DIR] [--audio-device VALUE] [--monitor
VALUE] <mode>`:

| Option or mode | What it does |
|---|---|
| `--audio-device VALUE` | plays the sound on one output device: sets `NX2SARIA_AUDIO_DEVICE` before the settings are read, so it wins over `nx2saria.ini`'s `audio_device =` (or an existing `nx2navi.ini`). VALUE is `default` (the system default device), a 1-based index as `--list-audio-devices` prints it (devices sorted by name), or part of a device name, case-insensitive |
| `--list-audio-devices` | prints each playback device's index, name, channels and frequency, marks the system default and shows which device `NX2SARIA_AUDIO_DEVICE` picks; needs no game files |
| `--monitor VALUE` | opens the game window on one monitor: sets `NX2SARIA_MONITOR` before the settings are read, so it wins over `nx2saria.ini`'s `monitor =` (or an existing `nx2navi.ini`). VALUE is `auto` (default), `primary`, a 1-based index as `--list-monitors` prints it, or part of a monitor's name (on Windows also of its GDI device name, DISPLAY3), case-insensitive. Wayland places a windowed window itself: only fullscreen opens on the monitor |
| `--list-monitors` | prints each monitor's index, name (on Windows also its GDI device name), desktop rectangle, primary flag and DPI; needs no game files |
| `--selftest` | layout, dispatch table and host-function registry; needs no game files |
| `--probe-mods` | the mods folder scan, order and switches (`runtime/patches/mods.c`); needs no game files |
| `--probe-loader` | loads and relocates the modules, checks them against the measured 1.2.1 values |
| `--probe-kernel`, `--probe-ipc`, `--probe-services`, `--probe-fs` | host probes |
| `--probe-discord` | the Rich Presence IPC exchange; skips without a Discord client; no game files |
| `--probe-dpi` | the process DPI awareness (per-monitor v2) and the primary monitor's size; no game files |
| `--probe-gfx-abi` | NVN object layouts against the reserved sizes of `runtime/nvn/nvn.h`; needs no game files |
| `--probe-nvn`, `--probe-nvn-tex`, `--probe-window`, `--probe-shader`, `--probe-vkmem`, `--probe-vkexec` | graphics probes |
| `--probe-prewarm` | the boot's pipeline stages from the caches next to the executable, the memory each leaves and what a compute pipeline's first dispatch costs; skips without recorded shaders, runs no game |
| `--probe-ffx-cache` | one FSR 3.1 upscaling context and where its pipelines came from (the pipeline caches kept across runs); skips without `amd_fidelityfx_vk.dll`, runs no game |
| `--probe-astc` | the compute ASTC decoder against `astc_decode_ldr()`; needs no game files |
| `--probe-meshfb` | the mesh shader fallback: mesh programs drawn through VK_EXT_mesh_shader and through compute passes, the pictures compared; needs no game files (`runtime/gfx/mesh_fb_probe.c`) |
| `--probe-fsr3` | FSR 3.1 upscaling with the built-in passes (`runtime/vk/vk_fsr3.c`) on a synthetic scene; no game files |
| `--probe-fsr3fg` | FSR 3.1 frame generation with the built-in passes (`runtime/vk/vk_fsr3fg.c`) on a synthetic scene |
| `--probe-zstd` | host zstd streams (`runtime/hle/ov_zstd.c`) on frames made here; needs no game files |
| `--probe-input` | gamepad polls on a virtual gamepad, also while SDL's update takes long; needs no game files |
| `--probe-movie` | the host movie decoders on one RomFS movie (VP9 `movie::Decoder`, AV1 `movie::VideoDecoder`); skips without game data |
| `--boot` | runs the game to the first NVN call |
| `--seconds S` | runs the game (implies `--boot`'s start-up) until the world clock (game time since the loaded save's first frame) passed S seconds, then exits 0 at that present |
| `--shot PNG` | with `--seconds`: saves that frame; the environment's `NX2SARIA_GPU=null|vulkan` selects the back end |
| (no mode) | runs the game |

### The log file

How the log is written and what its lines look like: [logging.md](../logging.md).

A game run whose stderr is not a file already (started from Explorer, a shortcut or a console) also writes its log to
`logs\nx2saria.log` next to the game executable; existing `nx2navi.log` files are left in place for reports and older
installs. `run_game.py` and `2> file` keep their own file. Each start rotates `nx2saria.1.log` to `nx2saria.4.log`
(`LOG_KEEP` 5). A file stops at 64 MiB (`LOG_CAP`; `NX2SARIA_TRACE` can write that much). Lines from before `log_open()`
wait in a static buffer: `mem_init()` runs first so that nothing else lands in the guest address space, and an earlier
open would put the CRT's heap there.

The end of the run still reaches a file past its cap: once the process is ending, up to `LOG_END_RESERVE` more bytes are
written, counted on their own (`g_log_tail`: the bytes of the end the file was given past `LOG_CAP`). The process is
ending when `rt_log_ending()` ran (`g_log_ending`), which is called at the start of an orderly end
(`runtime/gfx/gfx_present.c` `rt_exit_begin()`), of the frame-time summary (`runtime/diag/perf.c`
`perf_summary_flush()`), of a fatal error (`runtime/dispatch.c` `fatal_end()`), of the crash report (`crash_handler()`),
of the lost device's watchdog (`runtime/vk/vk_device.c` `lost_wait_exit()`), and in `plat_exit()` (`log_sync_ending()`).
A `trace = perf` session reaches the cap in about 90 minutes, and every line after it, the exit summary, "frametime:
exit", the stutter map and FATAL lines included, went to stderr only, which a game started from the launcher or Explorer
does not keep. `rt_log_sync()` alone does not open the reserve: a bug capture calls it in play (`runtime/diag/bugcap.c`
`write_log_tail()`), and the play's lines after it would fill the reserve before the end came.

`g_err_lock` guards stderr's writes (`log_sink()`). `g_log_final`: `rt_log_sync()` has run: an exit, a fatal error, a
crash report or a bug capture (`runtime/diag/bugcap.c`).

### `log_sink()`

The log's destinations: the file first, then stderr. `runtime/logq.c` calls it: from the log thread in a game run, else
from the thread that logs. A write to stderr can block for as long as its reader likes: a console window holds every
write back while a selection is made in it, and the log thread then stays in that write. The file has the lines by then,
and the lines of the process's end (`rt_log_sync()`) skip stderr while another thread is stuck in a write to it, so the
exit and FATAL lines still reach the file. stderr goes through `plat_stderr_write()`, which holds no lock of the C
runtime's stderr stream, so `fflush(stderr)` and `plat_exit()`'s `fflush(NULL)` do not wait for a stuck write either.

The first write past the cap says so, once. Without the file stderr is the log (`run_game.py`, `2> file`), and it is
waited for.

### `log_sync_ending()`

`plat_exit()`'s first step in a game run (`plat_exit_hook()`): the process ends here, so its last lines reach the file.
With the log privacy on it writes how often `runtime/log_privacy.c` acted (the start's "log: personal folders" line said
it is on).

### `console_closed()`

The console window was closed, or Ctrl+C or Ctrl+Break pressed in it (`runtime/platform/platform.h`
`plat_console_setup()`): the end of a closed game window, which writes the settings, the caches and the saves and logs
the exit (`runtime/gfx/gfx_present.c` `gfx_window_closed()`). Left to Windows, a closed console ended the process with
nothing saved.

### `rt_log_focus_lost()`

The window lost the focus (`runtime/gfx/gfx_present.c` `gfx_window_focus_changed()`): a process ended from outside soon
after lost the lines the log thread was still gathering, and both unexplained ends of 2026-11-13 came seconds after one.
The log thread writes them at once, and the module list for the note of an ExitProcess is brought up to date.

### `rt_log_raw()`

Every line of the log comes through here, and a personal folder in it is written short first (`runtime/log_privacy.c`),
so the file, stderr, the queue the exit note reads and the crash report all have the short form. A line without one,
nearly all of them, goes on as it is. One with one is copied on the stack; a piece larger than the copy (a debug string
of the game's, `runtime/hle/svc.c`) goes on in parts.

### `log_open()`

Opens the log file for a game run (`game` = 1) and writes the buffered lines to it; anything else drops them. Runs on
the main thread before the boot starts other threads.

- The rotation: `nx2saria.3.log` to `nx2saria.4.log`, and so on down to `nx2saria.log` to `nx2saria.1.log`.
- An ExitProcess nobody of ours asked for leaves a line in the file (`plat_exit_note_setup()` of
  `runtime/platform/platform.h`).
- The time zone is read here: its first use reads a file, and a fatal error's report must not be the first
  (`runtime/log_stall.c`).
- A game run writes its log from the log thread (`runtime/logq.c`), whether the lines go to a log file or to stderr
  alone (`NX2SARIA_LOG_ASYNC`).

### `parse_trace()`

`NX2SARIA_TRACE` has no default: nothing is traced when unset.

### `fnprof_hit()`

One of the runtime services the generated code calls (`runtime/cpu.h`). `g_fnprof` is switched on by
`runtime/gfx/resolution.c` until the game sets its render size (`res_fn_hit()`).

### `rt_ticks()`

CNTPCT_EL0: the Switch counter runs at 19.2 MHz. The scale (`tick_scale`) is set before `main()`, so no thread reads it
half written.

### `lifted_function()`

The lifted function that contains a host code address, by the nearest function start below it; 0 outside the lifted
code. The lifted code is one range with the host code outside it (`runtime/hle/kernel.c` `ap_init()` checks that); an
address above the last function start is taken for host code, which costs only the last function's body.

### `foreign_hook()`

A module another program put into the game's process: the Vulkan layer or the hook of a recorder, an overlay, a capture
tool or a post-processing injector, by the words such files carry in their names. A crash inside one is that program's:
one report was an access violation in a recorder's layer the moment the present mode changed and the swapchain was made
again, another an injector's layer reading a null pointer at the end of every shader preparation. Vulkan layers are
mostly named `VkLayer_<name>.dll`, and a recorder's Vulkan hook and a driver's capture tool carry names of their own
(the words in the function). The validation layer is left out: the game loads it itself when `NX2SARIA_VK_VALIDATION`
asks.

### `crash_module_note()`

What the crash report says of the faulting module (`CRASH_NOTE_*`). A fault on the thread that hands the frame limit to
RivaTuner Statistics Server (`runtime/pacing/rtss_limit.c`, the "rtss" thread) is in the game's own calls of RTSS's
library or around them, whatever module it is in (the one such crash faulted in ntdll): the note names that and the
setting that keeps the game from calling it. On other threads RTSS's library is its overlay, which RTSS put there:
`foreign_hook()`'s note.

### `crash_handler()`

`mode_done` is set when `main_run()` has returned: the mode's work is done and saved, and all that still runs is the C
runtime's exit and, inside ExitProcess, the detach call of every library in the process, the graphics driver's and those
other programs put there. A fault in one of them is not a failure of the run. On one PC an injector's Vulkan layer
faulted in its detach at the end of every `--warm-shaders` and `--prepare-shaders` (four runs over three versions): each
wrote a FATAL after "variants built into the driver cache" and ended with code 71, so the launcher took a finished
preparation for one that failed. `crash_handler()` ends such a process with the code its mode returned. A fault in this
executable's own code on the way out (an atexit handler) is still reported.

The steps, in order:

- `rt_ending_enter(71)` is shared with the fatal error's end (`runtime/dispatch.c` `rt_ending_enter()`): another
  thread's fault or fatal error waits there while this report is written; a fault inside this thread's own report ends
  the process at once.
- `rt_log_ending()`: the report reaches the log file past its cap (`log_sink()`).
- `vk_pipeline_crash_note()` first: the next start tells a crash in the boot's builds from a stopped start by it.
- `rt_log_sync()`: the lines before the crash, then the report, each written as it is made.
- What the thread was doing (a pipeline build names its shaders, `plat_crash_context_get()`): the stderr note the
  platform's exception handler writes misses the log.
- For abort and invalid parameter: where the host code called it; for a fault in another module: the host calls that led
  into it.
- The time stamp comes after the report's body: the zone is read at the start (`log_open()`), so this does not wait for
  a lock or a file.
- `rt_fatal_notes()`: the card, its driver and the memory settings in effect (`runtime/vk/vk_device.c` `fatal_note()`).
- `fs_save_crash_wait()`: a save the game already reported as done finishes first (bounded; `runtime/hle/fs_host.c`).
- After the report, `rtss_limit_crash()`: RivaTuner Statistics Server's profile goes back at the next start, from the
  line the run keeps for it in `rtss-profile.txt` (`runtime/pacing/rtss_limit.c` `rtss_limit_crash()`), with no call
  into RTSS's library from a process that failed; only where that line could not be written is it put back here, and
  never from RTSS's own thread.
- `rt_rumble_stop_bounded()`: a controller on HD rumble stops (`runtime/dispatch.c`: 300 ms at most, SDL may be the
  crash).
- `prof_sampler_write()`: `NX2SARIA_PROFILE`: what was sampled until the crash (it gives up after 2 s if the sampler
  hangs).
- The process ends with code 71.

### `check_cpu()`

The self-test covers the layout, the dispatch table and the host-function registry, without game files. `RT_CPU_LEVEL`
is the instruction set `build_game.py --cpu` compiled for: v3 (AVX2/FMA/BMI2, the default) or v2 (SSE4.2/POPCNT).

### `check_layout()`

The layout: ascending, no overlap, below 2^39, and inside the window `mem_init()` reserved.

### `check_modules()`

Every function start dispatches to its own function. The word after a start (unless that is a start too), a misaligned
address and the word below the module dispatch to nothing.

### `check_memory()`

The memory SVCs without modules: heap, physical memory in the alias region, a stack move, QueryMemory, GetInfo. The
over-limit request is one page more than the whole pool (GetInfo 6), so it is over the limit whatever the title's pool
is.

### `probe_crash()`

`--probe-crash-call` and `--probe-crash-null` crash on purpose, to check the report of a fault outside this executable.
`call`: `strlen` in the C runtime library (`ucrtbase.dll`, `libc.so.6`) reads address 8, the way a driver faults inside
a call the host made; the report names the library and the host frames that called it. `null`: a call through a null
function pointer, which leaves rip at 0, outside every module. Both end with exit code 71 and a FATAL line in the log.

`--probe-crash-exit` is the fault of `--probe-crash-call`, made by an atexit handler after the mode has returned, the
way a library another program put into the process faults in its detach. The process must end with the mode's own code
(`PROBE_EXIT_CODE`, 3) and an "exit:" line, with no FATAL (`mode_done`, `crash_handler()`). `tests/exit_fault_check.py`
runs it.

### `portable_path()`

Paths for a copied folder (`tools/package_build.py`). Per path: `NX2SARIA_EXEFS`, `NX2SARIA_ROMFS`,
`NX2SARIA_PROFILE_DIR`, else the folder next to `nx2saria.exe` (`exefs` with `main.npdm`, `romfs` with `romfs.bin`, any
`profile` folder) if it is there, else the path baked in at build time. A relative value is relative to `nx2saria.exe`'s
folder. Without a file that must be there, the folder itself is the sign.

### `log_libs()`

The SDL this process loaded and where from, and the form SHA-256 runs in (`runtime/lib_origin.h`). The line buffer is
2048 bytes because `rt_log()`'s own line is.

### `main()`

A game run never comes back here: it ends in `plat_exit()` (`runtime/boot.c`, `runtime/gfx/gfx_present.c`). Every other
mode returns its code.

- `plat_stack_guarantee()`: a stack overflow on the main thread still gets its crash report
  (`runtime/platform/platform.h`).
- Once `mode_done` is stored only the exit runs.
- `plat_exit_orderly()`: the C runtime's exit ends the process with ExitProcess, so there is no note of an end nobody
  asked for.

### `game_run()`

1 when the command line names the game itself: no mode, and every argument an option the game takes with its value
(`--seconds` and `--shot` run the game). `mem_init()` needs it before the parse, for the settings' start
(`runtime/cfg.h` `cfg_start()`: only the game probes the GPU and writes the preset's state file); a command line the
parse refuses counts as a tool mode.

### `main_run()`

- `log_privacy_init()`: the personal folders to write short in the log, from the environment, before its first line.
- `mem_init()` first: it claims the guest address space before anything else can land there, and reads the settings
  file.
- `log_privacy_start()`: the switch (`log_short_paths`) and the line that says how paths are written.
- The `version:` line names the build that wrote the log, at its top: an attached log from another install or version
  shows at a glance.
- A restart from the settings menu (`runtime/gfx/gfx_present.c` `gfx_window_closed()`) starts this process while the one
  it replaces is still exiting: its log, RomFS and saves are free once it is gone. The wait is 30000 ms; after it the
  line "restart: the previous process still runs after 30 s; starting anyway".
- Timed guest waits (`runtime/hle/kernel.c` `block_inner()`) sleep in whole milliseconds; at the default 15.6 ms tick a
  1 ms wait costs a full tick. `NX2SARIA_TIMER_1MS=0` keeps the default for comparison.
- The processor placement runs before any thread pool counts the usable processors. Not in the shader preparation modes:
  they translate and build on every processor the process was given, and the placement (`runtime/diag/perf.c`
  `cpu_placement()`) is for the game's own threads, so narrowing them to one die would only make the preparation take
  longer.
- `NX2SARIA_POWER_FULL=0` leaves Windows' power throttling of an unfocused window in place (A/B runs).
- `portable_paths()` runs before the parse: command-line paths still win.
- `--audio-device`: the same approach as every setting: `NX2SARIA_AUDIO_DEVICE` is read from the environment first
  (`runtime/cfg/cfg.c` `raw_get()`). `cfg_init()` has read the settings by now, so the variable goes in through
  `cfg_setenv()`, which makes the lookups read again. An empty value would remove the variable (`plat_setenv()`) and
  hand the choice back to the settings file, so "default" is the spelling for the system default.
- `--monitor`: the settings read `NX2SARIA_MONITOR` from the environment first (`runtime/cfg/cfg.c` `raw_get()`);
  `cfg_setenv()` as above. An empty value would remove the variable and hand the choice back to the settings file, so
  "auto" is the spelling for "no monitor choice" on the command line.
- The console of a game started directly: no QuickEdit to stop the log, and its close saves (`console_closed()`).
- A game run then calls, in order: `mem_start_note(0)` (a warning when the machine has less memory free than the game
  takes); `session_summary_start()` (`logs/session-summary.json` at the end, `runtime/session_summary.c`; not in tool
  modes); `hang_watch_start()` (a report in the log when the frames or the GPU stop, `runtime/diag/hang_watch.c`; never
  in tool modes); `cfg.boot = cfg.seconds > 0` (`--seconds` starts the game like `--boot`, with its checkpoints);
  `vk_driver_cache_env()` (before the threads that follow and `rt_boot()`'s, any of which may load the Vulkan driver);
  `discord_init()` (the Rich Presence thread, `runtime/discord_rpc.c`; it reads the settings file itself and idles while
  off); `tracker_api_init()` (a map tracker's read-only view of the game's memory, `runtime/diag/tracker_api.c`; logs
  whether it is on); `rt_boot()`.
- `--probe-dpi` checks the Win32 DPI awareness of `runtime/nx2saria.manifest`.
- `--probe-movie` sets the null back end: the AV1 decoder tells the graphics back end of each frame it writes, and there
  is none here.

## `runtime/boot.c`

The boot driver: the guest main thread runs the game's own `nn::init::Start` with three host callbacks, the second of
which runs DT_INIT of main, subsdk0 and sdk; Start then calls nnMain. The file's first two lines are a licence notice
and stay in the code.

`--boot` checks two checkpoints on the way and ends at the first nvnBootstrapLoader call, at the return address of each
in main:

| Checkpoint | 1.1.1 | 1.1.0 | 1.2.1 | 1.4.3 | Where |
|---|---|---|---|---|---|
| SetFocusHandlingMode (`CHECKPOINT_RET`) | main+0x1202d44 | main+0xeb8c14 | main+0xe7a954 | main+0xbdb48c | in nnMain; every module's constructor flag reads 2. 1.1.1's SDK calls it first, from `nn::oe::InitializeApplet` before DT_INIT; a call from outside main is not the checkpoint |
| nvnBootstrapLoader (`NVN_RET`) | main+0x346bf8c | main+0xebcdec | main+0xe7eb2c | main+0xbe0360 | in the graphics bring-up; logs the run statistics, exits 0 |

The return addresses come from the analysis of each build's main; 1.1.1's are the x30 both host functions see in a
normal start. With `--seconds S` the run continues past that call; `rt_frame_presented()` ends it once the world clock
passed S.

- `CTOR_DONE`, 2: a module's constructor flag after its DT_INIT ran the init_array.
- `InitCount` and the `inits_*` tables: the init_array sizes (1.1.0 and 1.2.1: counted in the modules; 1.1.1 and 1.4.3:
  DT_INIT_ARRAYSZ). The four tables carry the same rows, so `cb_module_inits()` walks them by the size of one.

### `ctor_state()`

The constructor flag is a symbol of the game's that the loader resolves; a title without it (or with a different name)
reports "done" rather than dereferencing a null address.

### `check_npdm()`

`main.npdm` META: the main thread's priority (+0xE), core (+0xF) and stack size (+0x1C). `runtime/hle/kernel.c` builds
the main thread with the 1.2.1 values; a different npdm means a different game build.

### `cb_loader_ready()`, `cb_module_inits()`, `cb_module_finis()`

The three callbacks of `nn::init::Start` (rtld's job on hardware). DT_INIT registers the module with musl
(`__nnmusl_init_dso`) and runs its init_array; the constructor flag reads 2 afterwards.

In `cb_module_inits()`:

- `kern_seed_tls()`: the SDK clears the thread-local region during init; this puts the TLS back where it reads it.
- The four "boot: sdk GOT[...]" lines log where the sdk's lock thunks actually land once the loader has resolved them:
  the words at sdk+0xe8fd20, +0xe8dff8, +0xe8f538 and +0xe91200.

### `rt_boot_event()`

The checkpoints declared in `runtime/host.h`.

- 1.1.1's SDK sets the focus handling mode in `nn::oe::InitializeApplet`: a return address outside main is logged as
  "outside main: not the checkpoint".
- With `--seconds` the run goes on into the world past the "nvn" event.
- At the end of `--boot` other guest threads are still running; nothing of theirs needs an orderly end, so the process
  exits at once.

### `rt_write_png()`

For `--seconds S` and `--shot`: a PNG with stored (uncompressed) deflate blocks: no compression library, and the shot is
only read by tools. `rt_write_png()` (declared in `runtime/vk/vk.h`) is shared with the `NX2SARIA_DUMP` frame dump
(`runtime/vk/vk_dump.c`). Each row's filter byte is 0, none.

### `save_shot()`

With the asynchronous replay the presented frame may still be queued for the replay, so the back end finishes first.

### `series_shot()`

`NX2SARIA_SHOTS=lo-hi:step [NX2SARIA_SHOTS_DIR=dir]`: every step-th present in [lo, hi] saved as `<dir>/f<n>.png` (n the
present), box-filtered to 480x270; `NX2SARIA_SHOTS=w<lo>-<hi>:<step>` takes them by world-clock seconds instead, the
first present at or after each step (full size with `NX2SARIA_SHOTS_FULL=1`), for finding short glitches. Pipeline
builds stay asynchronous, so the pictures show what a player sees. As in `save_shot()`, the back end finishes first when
the replay is asynchronous.

### `rt_frame_presented()`

A `--seconds` run ends at the first present after the world clock (game time since the save's first frame,
`frame_step_world_us()`) passed S seconds, so a run covers the same game time at any frame rate; any other frame is
counted by the back end only.

The end is the orderly end the window's close takes (`runtime/gfx/gfx_present.c` `rt_exit_steps()`), without the
settings file (`rt_exit_steps(0)`, no `RT_EXIT_SETTINGS`): a scripted run's end is no player's. A close already ending
the process keeps this thread in `rt_exit_begin()`.

### `guest_main()`

Steps 9 to 14 of the boot. The call is `nn::init::Start(main thread handle, arguments region, loader_ready,
module_inits, module_finis)`.

### `prewarm_main()`

- `NX2SARIA_TEST_PREPARE_SHADERS=1` is a test hook that runs the shader preparation here.
- Translations of a previous translator are made again in the background from here on, after the boot's own translations
  (`gfx_shader_carry_start()`). Not for a `--shot` image: which translation a shader gets would then depend on the
  pass's timing.

### `boot_wait()`

`rt_boot()`'s wait for the prewarm, polled every 50 ms. A warm start is done in about half a second and shows nothing.
When the prewarm translates shaders (a new translator, GPU or driver, or an emptied cache) or runs past `BOOT_SCREEN_MS`
(1000), the window shows the boot screen with its progress at 10 frames a second, and a black frame once it is done.
`cap_ms` (`NX2SARIA_PREWARM_WAIT_MS`) bounds the wait, 0 none. Without progress for `BOOT_STALL_MS` (60000; a translator
or driver that hangs) the guest starts anyway; the prewarm then goes on in the background. After the screen the window
stays black, not on the last count, until the game's first frame.

### `rt_boot()`

- `fs_host_lock_profile()`: a second copy of the game stops here, before the prewarm.
- `vk_pipeline_async_enable()` is skipped for a `--shot` image, which must not depend on build timing.
- Shaders and pipelines recorded in earlier runs are made ready while the modules load; the guest waits for them.
- `mods_exefs_switch()` comes after the patch sets' settings: an enabled mod's ExeFS patches on. `heap_retry_start()`
  comes after a mod that replaces the set `heap_retry` turned it off; `changebank_hold_start()` and
  `camera_clearance_start()` the same for the sets `changebank_hold` and `camera_clearance`.
- `wide_init()`: the game's widescreen sets (`recomp/patches-v111.txt`).
- `cfg_log_unsupported()`: the settings this build ignores.
- `cheats_init()` after the patch sets: a cheat's set follows its setting.
- `boot_map_init()` before the guest reads its first file: what it reads of the title and the save
  (`runtime/patches/boot_map.c`).
- The guest waits for all of the prewarm (`boot_wait()`): a warm start is ready in about half a second; a cold one (a
  new translator, GPU or driver) translated 2,987 shaders in about 11 s and then builds the recorded pipelines, with the
  boot screen showing its progress; after a driver change also the compute pipelines and the driver's cache of stage
  libraries (`runtime/vk/vk_pipeline.c` `bf_decide()`). Started early instead, the game would meet those shaders in play
  and wait for each one there. `NX2SARIA_PREWARM_WAIT_MS` caps the wait (0, the default: none); past it the prewarm goes
  on in the background. A `--shot` image waits for all of it without the screen, so that it does not depend on build
  timing.

## `runtime/loader.c`

NSO loading at the fixed layout bases, dynamic linking with host overrides, the nnMain identity check, sealing, and
`--probe-loader`. The file's first two lines are a licence notice and stay in the code.

The lifted C was generated from the unrelocated words of .text and .rodata and embeds absolute guest addresses, so each
module must sit at its layout base and relocations may only write the data segment.

- `hostfn_register_observer()` is declared here because `runtime/hostfn.c` defines it for the loader only.
- The `RES_*` values say how a symbol of a relocation was bound.
- `Mod`: `seg_off[3]` and `seg_size[3]` are text, rodata, data (data without bss); `relr` is DT_RELR (1.4.3), packed
  RELATIVE relocations; `hash` and `hash_mask` index the defined symbols by name; `res_plain` and `res_jump` are per
  symbol (a JUMP_SLOT may bind an observer); `funcs` and `nfuncs` are the defined functions sorted by value
  (`ldr_describe()`); `plt_slot`, `plt_stub` and `nplt` are the PLT stubs sorted by GOT slot (`ldr_plt_stub()`), built
  on first use; `kinds[]`, `sym_import` and `sym_own` are statistics.
- `ovr_used` and `obs_used` are per linked table entry, with probe overrides counted separately.

### `find_symbol()`

The defined symbol in load order (main, subsdk0, sdk); `*which` is its module index.

### `parse_dynamic()`

With DT_HASH the symbol count is `nchain`. With DT_GNU_HASH only (1.4.3 sdk and subsdk0) it is one past the last symbol
the highest bucket's chain reaches.

### `decode_ctor_flag()`

The constructor state flag DT_INIT checks: ADRP xN at DT_INIT+16, LDR wM, [xN, #imm] at +20 or +24.

### `unresolved_stub()`

A strong import nothing defines: calling it is fatal with its name (expected count 0).

### `resolve()`

The order: observer (JUMP_SLOT only), override by name, own definition, other modules in load order, weak gives 0,
strong gives the named fatal stub. Overrides also replace a module's own symbols.

### `relocate_table()`

S + A for every symbolic type (the AArch64 ELF ABI), JUMP_SLOT included where the symbol is a guest function; linkers
write 0 there. A host function's address takes no addend (an offset into a host trampoline means nothing), so such an
addend is logged and dropped, as is one on an unresolved weak symbol (address 0, the guest's own test for an absent
function).

### `relocate_relr()`

DT_RELR: an even entry is an offset to relocate (the next bitmap starts after it); an odd entry is a bitmap of the 63
words that follow. Each relocated word holds its addend, as R_AARCH64_RELATIVE with an implicit addend.

### `count_imports()`

Import table statistics: every undefined named symbol, bound by guest definition (overrides aside).

### `verify_identity()`

Every non-JUMP_SLOT reference to nnMain holds the real address.

### `seal()`

.text and .rodata become read-only: guest code never executes from guest memory, and nothing may write them.

### `ldr_describe()`

The search finds the last function with value <= the offset.

### `find_plt()`

A PLT stub is `adrp x16, page; ldr x17, [x16, #slot]; add x16, x16, #slot; br x17`.

### `check_segments()`

For `--probe-loader`, whose expected values were measured on the 1.1.0, 1.2.1 and 1.4.3 modules; 1.1.0 shares subsdk0
and sdk with 1.2.1 down to every count. This check: .text and .rodata in guest memory equal the NSO, so relocations
never touched them.

## `runtime/hostfn.c`

Host functions at synthetic guest addresses: the `HOST_OVERRIDE` / `HOST_OBSERVER` linker tables, the registry behind
them, and the calls between host and guest (`rt_call()`, `rt_original()`). The file's first line is a licence notice and
stays in the code.

Every registered function dispatches through `hostfn_entry()`, which counts the call, traces the first one
(`NX2SARIA_TRACE=host`) and tail-calls the real function. The registry is fixed by `hostfn_seal()`, which builds the
dispatch table; guest threads start after that, so lookups need no lock.

- `MAX_HOSTFN`, 4096: `runtime/dispatch.c` holds `HOST_CAP / 2` host functions.
- PE/COFF: the linker sorts `.ovr$a` < `.ovr$m` (the entries, `runtime/host.h`) < `.ovr$z` by the text after '$'.
- ELF: the linker defines `__start_<section>` and `__stop_<section>` around a section named like a C identifier. They
  are weak: a build without observers has no such section, and both bounds are then NULL.
- `Slot`: `original` is, for observers, the guest definition `rt_original()` calls; `blocking` means it runs with the
  virtual core given up (`hostfn_blocking()`).

### `table_find()`

The empty `__asm__` on `start` and `stop`: they are separate objects, and it keeps the compiler from reasoning about
them.

### `hostfn_table_size()`

The linked `HOST_OVERRIDE` / `HOST_OBSERVER` entries (the loader reports how many it installed).

### `hostfn_register_observer()`

For the loader: an observer slot and the guest definition it wraps.

### `hostfn_tracked()`

With `kern_host_track`: the call counted in the thread's host depth. Not a tail call, so the host stack holds this frame
while `fn` runs; the jump out of `fn` (ExitThread) leaves the depth raised on a thread that ends.

`fn` may have switched fibers (SwitchToFiber is a host call) and this context may resume on another OS thread: the fiber
switch moved this context's depth to that thread, whose `RtThread` the fiber's `Cpu` now names.

### `hostfn_blocking()`

`nn::fs`: the call waits for the disk and the OS, and on the Switch it is an IPC request that blocks its thread, so the
thread gives its virtual core up for it. `NX2SARIA_CORE_DIAG` found the game's loader threads (priority 58) holding a
core through CloseFile and CreateCacheStorage while job workers and better threads waited 2-6 ms for it.

### `return_sentinel()`

A guest jump to the `rt_call()` return sentinel: guest code used x30 as data, which a lifted RET never does.

### `hostfn_seal()`

The `mods[16]` array is static because the dispatcher keeps it.

### `rt_call()`

`rt_fpcr_sync()` first: guest code may start here on a host thread or fiber that has not run it.

## `runtime/host.h`

The interface of the host runtime: guest memory, modules, kernel, IPC, file system and boot. Nearly every file of the
runtime includes it.

Guest addresses are host addresses. Every guest-visible region lies below 2^39 (IPC buffer descriptors carry 39 address
bits). The fixed parts of the guest address map:

| Part | Where | What |
|---|---|---|
| modules | `MOD_<X>_BASE` from the generated `layout.h`, from 0x70_0000_0000 | the game's modules |
| kernel region | `RT_KERNEL_BASE`, 256 MiB | TLR, TLS blocks, host objects the guest reads (`mem_kernel_alloc()`) |
| host functions | `RT_HOSTFN_BASE`, 1 MiB, never accessible | the synthetic addresses of host functions |

The stack, heap and alias regions are placed at start-up (`mem_layout()`).

### Logging and results

- `rt_log()` writes "[host] <msg>\n" to the log, in one write.
- The log is stderr plus, in a game run whose stderr is not a file, `logs\nx2saria.log` next to `nx2saria.exe`
  (`runtime/main.c` `log_open()`). `rt_logf()` and `rt_log_raw()` write to both without the prefix and the newline:
  crash reports, fatal errors, guest output.
- `rt_log_path()`: the log file, NULL when this run writes none.
- `rt_log_sync()`: writes the queued log lines now; later lines are written as they are made.
- `rt_log_ending()`: the process is ending: its lines reach the log file past its 64 MiB cap.
- `rt_log_focus_lost()` (`runtime/main.c`): the gathered lines written now, the module list noted again.
- `hang_watch_start()` (`runtime/diag/hang_watch.c`): the thread of `NX2SARIA_HANG_WATCH_S`, in a game run.
- `kern_wait_report()` (`runtime/hle/kernel.c`): the guest threads' lock and thread-end waits, for a hang report.
- `vk_sync_progress()` (`runtime/vk/vk_sync.c`): submissions issued and done, with no lock or device call.
- `rt_trace` comes from `NX2SARIA_TRACE=svc,host,ipc,fs,gfx,perf,sched,audio,present` (or `all`).
- `RT_RESULT(module, desc)` builds a Horizon result.
- `rt_sha256()` is the SHA-256 of a buffer. It calls `runtime/sha256.c` `sha256()`, which cannot fail.

### The profiler and the perf trace

`runtime/diag/prof_win.c` holds the sampling profiler of `NX2SARIA_PROFILE=<path>`; `NX2SARIA_TRACE=perf` is the frame
timing (the waits of the render thread). How to read its lines: [performance.md](../performance.md).

- `PERF_TEX`: texture uploads on the render thread (deswizzle, decode, staging), `PERF_ASTC` included. `PERF_HANDOFF`:
  the presenting thread's wait for the present thread to take the previous picture (`runtime/vk/vk_backend.c`
  `present_image()`), which is where the present thread's own pacing and a slow present call (an overlay's frame limiter
  inside it) hold the game.
- `prof_write()` is idempotent; `runtime/boot.c` calls it before `_exit`.
- `perf_begin()` gives the start time of a wait; pair it with `perf_end()`, which counts render-thread time only.
- `perf_end_any()`: the same for a wait that may run on any guest thread (the game's image acquire): the render and
  replay threads' as `perf_end()`, any other thread's added to the next present's frame (`perf_present()`).
- `perf_replay()`: replay (NVN to Vulkan recording) time on any thread, waits inside it included.
- `perf_replay_thread()`: the calling thread replays for the render thread: `perf_end()` counts its waits.
- `perf_main_thread()`: the calling thread is the guest main thread (`NX2SARIA_PROFILE_MAIN`).
- The `PERF_CNT_*` events, per 60-frame window: graphics pipelines finished by the background workers, draws skipped
  while pending, first texture uploads and re-uploads after CPU writes, draws skipped while a shader translates in the
  background, draws that waited for their pending pipeline and got it, draws into a target the previous frame did not
  render that waited for their pending shader or pipeline instead of being skipped (`runtime/vk/vk_exec_draw.c`
  `prepare_draw()`). The rest feed only the flight recorder (`runtime/diag/flight.c`): graphics pipelines created on any
  thread, texture images created, images created at an address where a live image of another descriptor already starts,
  write-backs done and skipped because the CPU rewrote the storage after the GPU render (`runtime/vk/vk_texture.c`
  `vk_texture_write_back()`).
- The `PERF_M_*` marks (`NX2SARIA_TRACE=perf`): work on any thread that a frame can wait for besides the render thread's
  waits above, summed per frame and per 60-frame window (`runtime/diag/perf.c` `perf_mark_add()`): RomFS reads, host
  zstd decodes, pipeline creates (`runtime/vk/vk_pipeline.c` `pipe_create_in()`), guest memory maps (heap size,
  MapPhysicalMemory), NVN memory pool initialize and finalize (`runtime/vk/vk_memory.c` `vk_pool_init()`,
  `vk_pool_fini()`), device memory VMA allocates (blocks and dedicated memory, `runtime/vk/vk_device.c`
  `timed_alloc()`), and DLSS and DLSS frame generation feature creates (`runtime/vk/vk_dlaa.c`). `perf_mark_begin()`
  returns 0 while the trace is off, so a normal run pays one branch per site. `PERF_M_UPWAIT`: the upscalers' waits for
  the GPU before they free what they let go of (`runtime/vk/vk_dlaa.c` `vk_upscaler_device_idle()`, and
  `vk_upscaler_gpu_wait()`'s timeline wait after a refused creation). `PERF_M_UPREL`: their releases of DLSS features
  and FSR contexts (`runtime/vk/vk_dlaa.c`, `runtime/vk/vk_ffx.cpp`, `runtime/vk/vk_ffx4.cpp`).
- `perf_mark_end()` does nothing when `t0` is 0; `perf_mark_add()` nothing while the trace is off.
- Segments and scenes (`runtime/diag/perf.c`): `perf_segment()` ends the running segment of presents at the next present
  with a line of its frame times (mean, 1% low, worst), the work marks above, process memory and VRAM, then the memory
  snapshot, and starts one named `name` (the input script's `MARK` lines, `runtime/input/input.c`). `perf_scene()` notes
  an area's load (`runtime/hle/romfs.c`, the area's static placement file) and takes the memory snapshot once no area
  has loaded for 10 s.
- `perf_area_load_ns()`: `plat_now_ns()` of the last `perf_scene()` (any trace setting), 0 before the first.
- `PerfSizes` and the "mem: sizes" lines (`runtime/diag/perf.c` `sizes_log()`): the entries and bytes of the caches,
  pools and tables no other line gives, every 600 presents and with each memory snapshot. Each subsystem's `*_sizes`
  function (weak no-ops in `runtime/diag/perf.c`) adds its own with `perf_size()`, taking its locks briefly on the
  present thread. A count or bytes of `UINT64_MAX` (`PERF_SIZE_NONE`) means not known; limit 0 means none. The functions
  and their files: `runtime/memory.c` `mem_sizes()`, `runtime/hle/kernel.c` `kern_sizes()`, `runtime/guest_setjmp.c`
  `setjmp_sizes()`, `runtime/hle/fs_host.c` `fs_host_sizes()`, `runtime/hle/romfs.c` `romfs_sizes()`,
  `runtime/hle/ov_zstd.c` `zstd_sizes()`, `runtime/cfg/cfg.c` `cfg_sizes()`, `runtime/vk/vk_memory.c` `vk_mem_sizes()`,
  `runtime/vk/vk_gpa.c` `vk_gpa_sizes()`, `runtime/vk/vk_texture.c` `vk_texture_sizes()`, `runtime/vk/vk_exec_draw.c`
  `vk_exec_sizes()`, `runtime/vk/vk_sync.c` `vk_sync_sizes()`, `runtime/vk/vk_usnap.c` `vk_usnap_sizes()`,
  `runtime/nvn/nvn_api_cmd.c` `nvn_cmd_sizes()`, `runtime/vk/vk_pipeline.c` `vk_pipeline_sizes()`,
  `runtime/gfx/gfx_shader.c` `gfx_shader_sizes()`.
- The `PERF_RP_*` kinds (`NX2SARIA_REPLAY_TIME=1`): how long the replay spends in each kind of op (`runtime/diag/perf.c`
  `perf_replay_op()` reports it with the frame-time summary). Draws and dispatches include their bindings and the vkCmd
  calls they record; binds are the state and resource binds between them; xfer is copies and clears.
  `perf_replay_op_at()` is as `perf_replay_op()`, also kept per frame for slow frames.
- `perf_replay_cpu()`: the replaying thread's CPU time over one submission's replay (`runtime/vk/vk_sync.c`).
- `perf_lock_timing_here()` (`NX2SARIA_LOCK_TIME`): this thread's lock waits go into the frame-time summary.
  `perf_lock_holds_on()` says whether `NX2SARIA_LOCK_TIME` is on.
- `perf_lock_hold()`: a lock held `ns` from `site` (`runtime/vk/vk_texture.c`, holds over 1 ms). `perf_qlock_hold()`:
  the queue lock held `ns` from `site` (`runtime/gfx/gfx_present.c`, 5 ms or more).
- `perf_rec_waits` is set by the back end (the totals of `runtime/vk/vk_rec.c`).
- `perf_cpu_start()`: the process's logical processors (`NX2SARIA_CACHE_DIE`, `NX2SARIA_ONE_PER_CORE`), at start.
  `perf_cpu_on_cache_die()`: 1 while the process runs on the die with the largest L3 cache alone (`runtime/diag/perf.c`
  `cpu_placement()`).
- `perf_summary_flush()` prints the frame-time summary of the running period (at exit); the 60 s lines come from
  `perf_present()`. `perf_summary_flush_try()` is the same at a fatal error, with the stutter map only if its lock is
  free.
- `PerfSession` and `perf_session_stats()`: the whole session's frame times (`runtime/diag/perf.c` `ft_frame()` keeps a
  histogram beside the 60 s period's), for the session summary (`runtime/session_summary.c`): 0 before the first counted
  frame. Read after the frame-time summary's `ft_stop()` (`perf_summary_flush()`, `perf_summary_flush_try()`), when the
  present thread adds no more frames. No lock, no allocation.
- `perf_focus_lost()`: the window lost the focus: the session's reports come at the next present, in play.
- `gfx_exit_log()`: the back end's lines for the exit summary (a weak no-op in `runtime/diag/prof_win.c`).
- `perf_gpu_wait()`: a wait for a GPU fence ended (any thread; the render thread's also counts as `PERF_FENCE`).
- `PerfGpu` is the GPU side of the 60-frame perf line, filled by the graphics back end when it set `perf_gpu_stats`.
  `gpu_ns` is the GPU busy time since the previous call (timestamp queries, overlaps counted once); `have_gpu` is 0 when
  the device has no timestamps. `gpu_parts` and `gpu_missed`: submission parts timed, and parts left untimed.
  `have_idle`: calibrated timestamps, so the idle split is filled: `idle_ns` the GPU idle between timed parts,
  `starved_ns` the part of it before the part was submitted, `queued_ns` submit to start; `idle_gaps` the gaps over 0.5
  ms and `max_gap_ns` the longest. `mirror_ns`: of `gpu_ns`, the copies into VRAM mirrors (`runtime/vk/vk_sync.c`
  `ts_submitted()`).
- `perf_note_wc()` and `perf_wc_recent()`: the write-combined memory of the process (device memory the driver maps into
  it) as a walk of the address space counted it, with the driver's device-local figure at that time
  (`NX2SARIA_MEM_CENSUS`: `runtime/vk/vk_memory.c` `census_main()`, and the commit census of `runtime/diag/perf.c`).
  `perf_wc_recent()` gives the last such figure when it is at most `max_age_ns` old, else 0.
- The frame-time summary's "census" and "report" parts (`runtime/diag/perf.c` `ft_frame()`). `perf_census_walk(1)` and
  `(0)` bracket a walk of the address space on another thread (`runtime/vk/vk_memory.c` `census_main()`): a frame that
  overlapped it has that much moved to "census". `perf_report_part()` adds the time the memory report took on the
  presenting thread (`runtime/vk/vk_memory.c` `vk_mem_census()`), any thread, to the next present's "report".
  `perf_census_walked_frame()`: the frame whose present walked the address space itself (`runtime/diag/perf.c`
  `census_check()`), `UINT64_MAX` before any, so a report of that frame skips its own walk.
- The performance overlay (`runtime/diag/perfmon.c`), `perf_set_overlay()`: mode 0 off, 1 fps, 2 full. While it is on,
  `perf_present()` hands every frame to `perfmon_frame()`; full also takes the event counts (`perf_count()`) and the GPU
  timestamps as `NX2SARIA_TRACE=perf` does.
- `perf_gpu_timing()`: GPU timestamps are wanted: `NX2SARIA_TRACE=perf`, the full overlay, or a feature that asked
  (`perf_gpu_timing_want()`, which `runtime/pacing/dynres.c` calls for GPU timestamps while dynamic resolution runs).
- `perf_gpu_busy()`: the GPU busy time of a timed submission part (`runtime/vk/vk_sync.c`, any thread).
  `perf_gpu_busy_total()`: its running total (the overlay's GPU time per frame).

### The end of the process

- `gfx_window_closed()` (`runtime/gfx/gfx_present.c`, declared for `runtime/gfx/gfx_wsys.h`): the end of a closed
  window. It does not return, but for its own thread coming back (`runtime/gfx/gfx_present.c` `rt_exit_begin()`).
- `rt_exit_begin()` and `rt_exit_steps()` (`runtime/gfx/gfx_present.c`): the orderly end every exit the game asks for
  shares (the window's close, ExitProcess in `runtime/hle/svc.c`, `--seconds` in `runtime/boot.c`). `rt_exit_begin()`: 1
  for the first caller; another thread waits there for good; 0 for that caller's own thread again. `rt_exit_steps()`:
  the saves and the put-backs, the slow pipeline save late and the final save flush last; with `RT_EXIT_SETTINGS` it
  also writes the menu's changes to the selected settings file.
- `rt_exit_started()`: an orderly end has begun (the hang watch stops judging presents).

### AAPCS64 argument access

`arg_x()` and its siblings read a host function's arguments and write its results as AAPCS64 lays them out (section 2.2
of the host's design).

### Host functions

Defined in `runtime/hostfn.c`. Every host function is a `GuestFn` at a synthetic address `RT_HOSTFN_BASE + 16*i`. It
runs on the calling guest thread, may clobber x0-x18, v0-v7, v16-v31, must preserve x19-x29, sp, v8-v15, tpidr, and
returns to the guest BL site with a C return. On entry `c->pc` holds the function's own synthetic address.

`HOST_OVERRIDE(sym, fn)` and `HOST_OBSERVER(sym, fn)` are file-scope declarations; `fn` must be declared before. The
loader binds them by symbol name. The entries of every object file meet in one section per kind, which
`runtime/hostfn.c` walks: on PE/COFF ".ovr$m", which the linker sorts between the ".ovr$a" and ".ovr$z" markers of
`runtime/hostfn.c`; on ELF "nx2navi_ovr", which the linker brackets with `__start_nx2navi_ovr` and `__stop_nx2navi_ovr`.
An override replaces `sym` in every GOT/GLOB_DAT/ABS64 of every module. An observer is for JUMP_SLOT only; its `fn`
calls `rt_original(c)`.

- `hostfn_register()` gives the synthetic address; before `hostfn_seal()` only. The same (name, fn) pair returns the
  same address.
- `hostfn_override()` and `hostfn_observer()`: NULL if none.
- `hostfn_add_override()`: an override outside the linker table (probes); before `ldr_load()`.
- `hostfn_name()`: the name registered at a synthetic address, NULL if none.
- `hostfn_seal()`: builds the dispatch table (`rt_dispatch_init()` over every linked module); idempotent; registration
  ends here.
- `hostfn_calls()`: host-function calls so far, all threads. `hostfn_report()`: the call count per host function to
  stderr, busiest first.
- `hostfn_table_size()`: the linked `HOST_OVERRIDE` (0) or `HOST_OBSERVER` (1) entries. `hostfn_table_name()`: the
  symbol of linked entry `i`, NULL past the end.
- `rt_original()`: inside an observer, calls the guest definition with the current x0..x7 and returns x0. It uses
  `c->pc`, so call it before anything that changes `c->pc` (`rt_call()` restores it).
- `rt_call()`: host to guest, section 2.3 of the host's design. `n` <= 8 arguments, each read as `uint64_t`: cast every
  argument. It seals the dispatch table on first use, sets x30 = `RT_RET_HOST`, restores x30 and pc, returns x0, and
  does not touch sp.
- `RT_RET_HOST`, 0x70200ff000: the x30 sentinel inside the host-function window; a jump to it is a named fatal.

### Guest memory

Defined in `runtime/memory.c`.

- `RtMemoryInfo` has Horizon's layout.
- `mem_init()`: reserves every region; on a collision it re-runs the process (up to 20 times, `NX2SARIA_REEXEC`), then
  is fatal. It reads the settings (`game`: `runtime/cfg.h` `cfg_start()`).
- `mem_commit_checked()`: commits, or logs the native error, the range and the commit.
- `mem_start_note()`: a game's start: a warning line when the free commit is below the game's need
  (`runtime/mem_need.h`); 1 when it is.
- `mem_guest_census()`: the committed bytes per guest region (`NX2SARIA_MEM_CENSUS`).
- `mem_guest_ranges()`: the guest's ranges for a census, each with its name (the loaded modules one range each, all
  named "modules"), at most `max`; `mem_guest_census_log()` logs the bytes a walk found committed in each
  (`committed[i]` for range `i`).
- `mem_lazy_heap_census()`: the census's lazy heap line (`NX2SARIA_HEAP_LAZY` only). `mem_lazy_heap_report()`: with
  `NX2SARIA_TRACE=perf`, the lazy heap's faults since the last line.
- `mem_guest_usage()`: the game's own memory for the memory report (`runtime/vk/vk_memory.c` `owner_log()`): its heap's
  size, the part of it committed (the size itself unless the heap is lazy), and the physical memory it mapped. No lock:
  a log may read a figure late.
- The SVCs: `mem_set_heap_size()` 0x01, `mem_set_permission()` 0x02, `mem_set_attribute()` 0x03, `mem_map()` 0x04,
  `mem_unmap()` 0x05, `mem_query()` 0x06, `mem_map_physical()` 0x2C, `mem_unmap_physical()` 0x2D.
- `mem_readable()`: every page readable. `mem_map_gen()` changes with everything `mem_readable()` answers from (mapping,
  heap, permissions). `mem_read_hold()` is a `mem_readable()` that keeps the range mapped until `mem_read_release()`.
- `mem_info()`: the GetInfo memory ids 2-7, 12-17, 21, 22, 28; 0 if not a memory id.
- `mem_kernel_alloc()`: zeroed, guest-visible, in the kernel region; `align` <= 64 KiB. `mem_kernel_free()`: TLR/TLS
  reuse after thread exit.
- `mem_written_pages()`: GetWriteWatch(RESET): fills `pages[]` (4 KiB addresses) with the pages of [addr, addr+size)
  written since the last call, returns their count (at most `max`; a page past `max` stays reported next time). The
  range must lie inside the stack/heap block or the alias region (write watch); otherwise it returns `UINT64_MAX`.
  `mem_written_peek()` is the same without the reset: the bits stay for the next `mem_written_pages()` (verify checks
  only).
- `mem_watched()`: [addr, addr+size) lies inside one of those two ranges, so one `mem_written_pages()` call can answer
  for all of it.
- `mem_freeze()`: makes the 2 MiB region at `a` (2 MiB aligned, watched, committed PAGE_READWRITE) read-only until a
  write or `mem_thaw()` thaws it; 1 frozen (or already), 0 refused. `mem_frozen()`: the 2 MiB region of `a` is frozen:
  no page in it changed since its freeze.
- `mem_thaw()`: makes the frozen regions over [a, a+n) writable again; before a kernel write (file read) into guest
  memory; it also commits pages the lazy heap left reserved.
- `mem_kernel_access()` (`NX2SARIA_HEAP_LAZY`): commits the reserved heap pages of [a, a+n) before the kernel reads them
  (a file write). `mem_untouched_run()`: the end of the run of heap pages at `a` that were never committed (a scan steps
  over it), else 0. `mem_host_readable()`: `plat_mem_readable()` that first commits the untouched pages of the lazy heap
  in [a, a+n), which read zero.
- `mem_freeze_stats()`: the regions frozen now; freezes and thaws since the last call. `hot_holds`: the quick thaws that
  held their region thawed ("Hot regions" in the notes of `runtime/memory.c`).
- `NX2SARIA_WB_PROBE=1` ("Probed regions" there): who reads what the GPU wrote back into guest memory.
  `NX2SARIA_WB_LAZY` ("Lazy write-back" there): pages the GPU wrote that come down only when the CPU touches them.
- `MemLazyCopy`: [`off`, `off + size`) of buffer `h0` into buffer `h1`. The `fetch` of `mem_lazy_enable()` makes the `n`
  copies after all the GPU work handed over so far, and waits for them; it is called on several threads at once.
- `mem_copy_readable()`: copies the ranges `src[i]` = {address, bytes} of guest memory whose pages read without a fault
  to `dst[i]`, `ok[i]` set.
- `mem_lazy_touches()`: 0 when the CPU can read [a, a + n) without a fault now.
- `mem_lazy_release()`: `rg[i]` = {lo, hi, buffer or 0 for any}: the armed parts come down.
- `mem_lazy_note()`: its state as a phrase, without a lock (a fatal error's line). `mem_lazy_pick()`: a test, the
  addresses of armed pages.
- `mem_probe()`: the 2 MiB regions inside [a, a+n) fault on their next access. `mem_probing()`: the probe is on.
  `mem_probe_report()`: logs the accesses since the last report.

### Modules

Defined in `runtime/loader.c`.

- `RtLoaded`: `text_end` and `data_off` are offsets from `base`; `init`, `init_array`, `init_array_size` and `ctor_flag`
  are absolute guest addresses, `init_array_size` in bytes.
- `ldr_load()`: load, relocate (overrides applied), seal; once.
- `ldr_module()`: the order is main, subsdk0, sdk.
- `ldr_symbol()`: an exported definition, 0 if none. `ldr_plt_stub()`: the PLT stub whose GOT slot holds `name`, 0 if
  none.
- `ldr_describe()`: "main+0x1234 (symbol+0x10)", "host <name>", or the address.
- `ldr_read_verified()`: the whole file, its SHA-256 checked against the generated `modules.c` (fatal on a mismatch);
  `free()` it.

Written by `tools/build_game.py` into `build/<version>/modules.c`:

- `rt_exefs_sha256[]`: "exefs_sha256" of `tools/data/expected-<version>.json`.
- `rt_app_pool_bytes`: the title's application memory pool (GetInfo 6).
- `rt_nvn_api_version[2]`: the `NVN_DEVICE_INFO_API_{MAJOR,MINOR}_VERSION` the title requires.
- `rt_game_version`: the game build: 111 (1.1.1, `tools/make_game.py`), 110 (1.1.0), 121 (1.2.1) or 143 (1.4.3).
- `rt_port_version[]`: `VERSION` at the build's root: 0.1.0, or 0.1.0+973c626 in a release.
- `RT_V110`: 1.1.0 has its own boot constants and data slots. `RT_V111`: 1.1.1 has its own boot constants
  (`runtime/boot.c`).

### Kernel

Defined in `runtime/hle/kernel.c`; its notes are in [hle.md](hle.md).

- `kern_cpu()`: the current guest thread's `Cpu`, NULL on host threads.
- `fs_save_crash_wait()` (`runtime/hle/fs_host.c`): the crash report's and `rt_fatal()`'s bounded wait for a save Commit
  under way.
- `kern_free_cores_note()`: for crash reports: logs that `free_cores = 1` lifted the virtual core gate, nothing when the
  gate is on.
- `kern_main_thread()`: the main thread object and `Cpu` for the calling host thread (`main.npdm`: 1 MiB stack at the
  top of the stack region, priority 44, core 0); on the main thread: its handle.
- `kern_main_stack()`: the main thread's stack, size 0 before `kern_main_thread()`; `runtime/hle/svc.c` reports it to
  QueryMemory, `runtime/memory.c` does not know it.
- `kern_run_on_guest_main()`: starts the guest main host thread, runs `fn` there, joins.
- `kern_core_away()` and `kern_core_back()`: give the virtual core up around a blocking host wait.
- `kern_checkpoint()`: a preemption point, at every SVC and indirect call.
- `kern_preempt_pending()`: a preempt request is pending for the thread (`Cpu.thread`): its `RtThread` starts with the
  flag.
- `kern_host_track` is set once before guest threads start (`NX2SARIA_CORE_DIAG`, `NX2SARIA_ASYNC_PREEMPT`): host calls
  (`runtime/hostfn.c`) and SVCs (`runtime/hle/svc.c`) count their depth in the thread's second int
  (`kern_host_depth()`), so the kernel knows whether a core owner runs game code at the top level. `kern_host_left()`:
  back at depth 0 with a preempt request pending.
- `kern_core_diag_on()`: `NX2SARIA_CORE_DIAG`. `kern_core_diag_frame()`: called by `perf_present()`, every present (it
  also logs the asynchronous preemption counters).
- `kern_sched_report()`: `NX2SARIA_TRACE=sched`: the per-thread time split, on present.
- `kern_handle_new()`: `kind` is `KERN_SESSION` or `KERN_TMEM` (`runtime/hle/ipc.c`). `kern_handle_hold()`:
  `kern_handle_get()` with `hold(obj)` run under the kernel lock (a reference the caller drops). `kern_handle_copy()`: a
  copy of the object, made under the kernel lock; 0 for a bad handle. `kern_handle_close()`: CloseHandle from the host
  side, any thread; fatal on a bad handle.
- `kern_event_new()` gives the writable handle. `kern_event_clear()`: a server-side clear (for example a drained message
  queue).
- `kern_bind_system_event()`: guest thread only. `kern_signal_system_event()`: any thread. `kern_clear_system_event()`:
  any thread; a bound, live event only. `kern_unbind_system_event()`: before freeing a host-memory event nothing signals
  now; destroys it on a guest thread (`c` set). `kern_system_event_live()`: in mapped guest memory, its state byte
  nonzero.
- `kern_svc()`: the thread and synchronisation SVCs; 1 if handled.
- `rt_guest_start_ns`: `kern_now_ns()` when the guest started (`runtime/boot.c`), 0 before.
- `rt_stats()`: `host_calls` is `hostfn_calls()`.

### IPC, file system, account, movies

- `ipc_svc()` (`runtime/hle/ipc.c`): SVC 0x15, 0x1F, 0x21, 0x22; 1 if handled. `ipc_handle_closed()`: CloseHandle on a
  `KERN_SESSION` or `KERN_TMEM` handle.
- `fs_init()` (`runtime/hle/romfs.c`, `runtime/hle/fs_host.c`): idempotent; the paths come from `rt_config()`.
- `account_uid()` (`runtime/hle/ov_system.c`): the local user's `nn::account::Uid` {lo, hi}, persistent in
  `<profile>/account.uid`; `runtime/hle/fs_host.c` names save data after it.
- `movie_tick()` (`runtime/hle/ov_movie.c`): at every present, on the presenting guest thread: hands decoded movie
  frames over when due, signals their events.

### Camera speed

`runtime/input/camera_speed.c`, `NX2SARIA_STICK_SENS`.

- `cam_speed_wanted()`: the factor `NX2SARIA_STICK_SENS` asks for, 0.1 to 3. `cam_speed_applied()`: the factor in
  effect: 1 until `runtime/hle/romfs.c` serves the pack.
- `cam_pack_build()`: `Pack/Actor/PlayerCamera.pack.zs` with the camera speeds multiplied, malloc'd; 0 means factor 1 or
  cannot (logged).
- `cam_pack_opened()`: called by `runtime/hle/romfs.c` when the game opened the camera's pack. `cam_pack_served()`:
  `runtime/hle/romfs.c` serves the multiplied pack. `cam_speed_tick()`: every present: a changed setting goes into the
  game's memory.

### Game resolution and widescreen

`runtime/gfx/resolution.c` (`NX2SARIA_OUTPUT_RES`, `NX2SARIA_RENDER_SCALE`) and `runtime/patches/widescreen.c`
(`NX2SARIA_GAME_ASPECT`).

- `res_heap_extra_bytes()`: added to the application pool (`mem_init()`), in 2 MiB steps. `res_heap_extra_kib()`: added
  to the HeapDef texture heap and its parent; 0 means none.
- `res_heapdef_build()`: the HeapDef table (`.byml.zs`) with the grown heaps, malloc'd; 0 means cannot (logged).
  `res_heapdef_served()`: `runtime/hle/romfs.c` serves the grown table. `res_heapdef_ok()`: 1 once it does; 0 after
  `fs_init()` means the game has the shipped heaps.
- `wide_init()`: after `rt_patches_init()`. `wide_present()`: each present: DLSS's render scale (`res_fixed`).
- `wide_lens_host()`: the lens-load site shared by `wide_aspect` and `fov`, patch page +0xe40.
- `wide_available()`: the generated C has the widescreen sets.
- `wide_picture()`: the picture the game draws (its scene is never larger). `wide_start_picture()`: the same as
  `wide_init()` will set it, asked before it (`runtime/hle/romfs.c`); 0 means 16:9.
- `wide_bars_init()`: called by `wide_init()`: centers the UI's letterbox margins on either axis.
  `wide_bars_screen_host()`: the site of the set `wide_bars` (patch page +0xef0, `runtime/patches/patches.c`).
- `wide_menu_open()`: the game's pause/map or system menu background is active.
- `menu_mouse_screen_host()`: the site of the set `menu_mouse` (patch page +0xee0, `runtime/patches/patches.c`).
- `wide_menu_bars()`: for `runtime/gfx/gfx_present.c`, each present: the margins outside the middle 16:9.
- `wide_scene_settable()`: the generated C has `res_fixed`: the scene's share can be set. `wide_scene_share()`: the
  scene's share of the picture now (0: the game's own). `wide_scene_wanted()`: what the settings ask for, before the
  heap's limit. `wide_scene_restart()`: a restart gives the larger scene the settings ask for.
- `wide_dynres_base()`: for `runtime/pacing/dynres.c`: the scene share to step from, -1 when an upscaler runs.
- `wide_heap_extra_kib()`: the texture heap's growth for the widescreen picture.
- `mem_heap_extra_max_kib()` (`runtime/memory.c`): the most `res_heap_extra_kib()` may be: with more the application
  pool passes the heap region and the game's own SetHeapSize is refused. A whole number of 2 MiB.
- `res_init()`: after `rt_patches_init()`: the patch page and the sets.
- `res_dlss_lod_bias()`: `NX2SARIA_DLSS` upscaling: the texture LOD bias (<= 0). `res_dlss_mode()`: `NX2SARIA_DLSS`
  changed while running: the new scene size.
- `dynres_frame()`: dynamic resolution: one presented frame (`runtime/vk/vk_backend.c`). `dynres_override()`: the DLSS
  mode dynamic resolution uses now, NULL for the setting's. `dynres_share()`: the scene share it uses now without an
  upscaler, 0 for the setting's.
- `res_fn_hit()`: called by `runtime/main.c` `fnprof_hit()`: catches the renderer at start.
- `res_present()`: at nvnQueuePresentTexture: applies a pending scene size.
- `res_sizes()`: the output and scene sizes in effect (the game's own until `res_init()` switched the sets on).
- `res_adjustable()`: the build has the output and scene patch sets.
- `res_is_16x9()`: within 1% of 16:9 (the test `res_init()` and `runtime/hle/romfs.c` share).
  `res_wants_wide_overlay()`: not 16:9 on a game version with the camera aspect patch.
- `res_wanted()`: the output size `NX2SARIA_OUTPUT_RES` asks for.
- `res_fov_live()`: `NX2SARIA_FOV` to the camera (set `fov`); on a settings change. `res_fov_factor()`: the 1.1.1 camera
  callback's multiplier.

### Code patches and the frame step

`runtime/patches/patches.c` and `runtime/pacing/frame_rate.c`.

- `rt_patches_init()`: after `ldr_load()`: the switches from the environment, the patch page committed; logs the sets.
- `rt_patch_set()`: the index of a set of the generated `patches.h`, -1 if absent. `rt_patch_enabled()`: the set is on
  (0 for -1). `rt_patch_disable()`: before the guest starts: a set needs another that is off. `rt_patch_switch()`: at
  run time (live settings): each site takes the new code the next time it runs. `rt_patch_page()`: the page the patched
  code reads (NULL without the table).
- `frame_step_init()`: after `rt_patches_init()`: the real-time step's first value. `frame_step_realtime()`: the
  real-time step is on: presents ignore the interval. `frame_step_cap_ns()`: with the real-time step, the shortest frame
  in ns. `frame_step_movie_hz()`: the movie rate `frame_step_cap_ns()` paces at; 0 means no movie.
  `frame_step_ceiling_ns()`: the shortest frame the game survives, whoever holds the limit.
- `ov_movie_live()` (`runtime/hle/ov_movie2.c`): a movie plays (`runtime/pacing/frame_rate.c` `movie_cap_hz()`).
- `frame_limit_adjustable()`: the frame limit setting changes the rate (always). `frame_free_running()`: no
  `fps60`/`realtime` patch set: the game steps by the clock.
- `frame_step_limit_hz()`: the frame rate the limit gives (overlay); 0 unknown. `frame_step_game_hz()`: the rate the
  game draws at under the limit; 0 unknown.
- The host sites of the patch page: `frame_rate_day_host()`, the time of day's seconds a frame (set `ostep`, page
  +0xf60); `frame_rate_water_host()`, the water's ripple steps a frame (set `ostep`, page +0xf50);
  `frame_rate_sim_host()`, water and floor surfaces' fixed steps (set `owater`, page +0xf20);
  `frame_rate_sound_gate_host()`, the sound job at whole steps (set `osound`, page +0xf10);
  `frame_rate_sound_step_host()`, the step the sound job hands on (set `osound`, page +0xf00);
  `frame_rate_horse_jump_host()`, the horse's jump blend (set `ohorse`, page +0xea0); `frame_rate_gohma_turn_host()`,
  Gohma's turn-rate bounds (set `ogohma`, page +0xe80); `animation_loop_host()`, a single-frame animation loop (set
  `ozora`, page +0xe90); `battle_music_flags_host()`, restores held battle music flags (set `obgm`, page +0xe70);
  `voice_language_host()`, refreshes saved voice-language changes (set `voice_language`, page +0xe60);
  `wide_balloon_host()`, keeps speech-balloon anchors with their speakers (set `wide_balloon`, page +0xe50).
- `frame_rate_animation_frames()`: whole clock frames for a single-pose loop. `battle_music_gate()`: phase 0's decision,
  under the sound gate lock.
- `frame_step_frames()`: the 60 Hz frames the game steps per update (set `ostep`), else 1. `frame_step_flight()`: for
  `runtime/diag/flight.c`: the step at a host present. `frame_step_presents()`: the game's presents so far: one per
  update of the game.
- `frame_step_world_us()`: game time since the loaded save's first frame, -1 before (`NX2SARIA_FPS_LIMIT`, live).
  `frame_step_ticks()`: game time since the first present in 60 Hz ticks (input scripts).
- `frame_step_present()`: at nvnQueuePresentTexture: the frame's display time and present interval (60 Hz ticks).

`runtime/pacing/rtss_limit.c`: RivaTuner Statistics Server takes the frame limit where it runs.

- `rtss_limit_want()`: the pacer's limit in millihertz: >0, 0 none; or why RTSS is kept out (the `RTSS_OUT_*` values of
  `runtime/pacing/rtss_decide.h`).
- `rtss_limit_movie()`: the rate of a movie that plays, 0 none: the pacer holds it.
- `rtss_limit_active()`: RTSS limits to the wanted rate: the pacer keeps the ceiling.
- `rtss_limit_state()`: for the menu: 1 RTSS limits, 2 its profile is read-only, 3 it let the game run past its limit or
  never showed it holds it, 4 kept out while a limit set in RTSS holds the game too.
- `rtss_limit_restore()`: before the process ends: the profile's old limit back. `rtss_limit_crash()`: a crash or fatal
  error: no call into RTSS, the next start puts the profile back (`rtss-profile.txt`).

### `rt_ab()`, `rt_knob()`

In-launch A/B measurement: `NX2SARIA_AB=<seconds>` splits the world clock (`frame_step_world_us()`) into blocks of that
much game time in the order A B B A (repeated); `rt_ab()` is 1 in the B blocks, 0 in A blocks, before the world clock
and without `NX2SARIA_AB`. `rt_knob(name, def)` reads the switch `NX2SARIA_<name>` once per call (`def` when unset): 0
off, 2 for "ab" (on in the B blocks only), 1 for anything else; callers keep it and test it with `RT_KNOB_ON`. The perf
line counts the B frames of its window (`tools/ab_summary.py` compares the windows that are wholly A or wholly B).

### Boot and the modes

`runtime/boot.c` and `runtime/main.c`.

- `RtConfig`: `romfs` is the directory with `romfs.bin` and `index.json`; `seconds` is `--seconds N` (0 means no limit);
  `shot` is `--shot PATH`; `gpu` is `NX2SARIA_GPU`.
- `rt_boot_event()`: the checkpoint log; "nvn" ends `--boot`.
- The modes of `nx2saria.exe`: `runtime/main.c` has run `mem_init()` and parsed the paths; each mode calls `ldr_load()`
  itself if it needs the modules, registers its host functions, then seals (`hostfn_seal()`, or implicitly at the first
  `rt_call()`). Each returns the process exit code.

Where each mode and start function lives: `runtime/audio/audio_mixer.c` `list_audio_devices()` (`--list-audio-devices`,
SDL3 on every window back end); `runtime/cfg/cfg_probe.c` `probe_cfg()`; `runtime/diag/dpi_probe_win.c` `probe_dpi()`
(Windows only); `runtime/gfx/gfx_window_win.c` `list_monitors()` and `runtime/gfx/gfx_window_sdl.c` `list_monitors()`
(`--list-monitors`); `runtime/patches/mods_probe.c` `probe_mods()`; `runtime/discord_rpc.c` `discord_init()` (the Rich
Presence thread, game runs only) and `probe_discord()` there too; `runtime/diag/tracker_api.c` `tracker_api_init()` (the
start line, and the listener when it is on) and `tracker_api_on()` there (`NX2SARIA_TRACKER_API` is on; `mem_init()`
reads it); `runtime/gfx/gfx_abi_probe.c` `probe_gfx_abi()`; `runtime/vk/vk_device.c` `probe_vkdevice()`;
`runtime/vk/vk_sync.c` `vk_sync_probe_devlost()` (`--probe-devlost`); `runtime/nvn/nvn_probe.c` `probe_nvn()` and
`runtime/nvn/nvn_tex_probe.c` `probe_nvn_tex()`; `runtime/gfx/gfx_window_probe.c` `probe_window()`;
`runtime/gfx/gfx_shader.c` `probe_shader()`; `runtime/gfx/gfx_shader_probe.c` `probe_census_dir()`
(`build/<version>/shaders` for the probes); `runtime/gfx/tex_probe.c` `probe_vkmem()`, `runtime/vk/vk_exec_probe.c`
`probe_vkexec()` and `runtime/gfx/astc_probe.c` `probe_astc()`; `runtime/gfx/mesh_fb_probe.c` `probe_meshfb()`;
`runtime/vk/fsr3_probe.c` `probe_fsr3()`; `runtime/vk/fsr3fg_probe.c` `probe_fsr3fg()`; `runtime/hle/ov_zstd.c`
`probe_zstd()`; `runtime/input/input.c` `probe_gamepad()`; `runtime/hle/movie_probe.c` `probe_movie()` (the host movie
decoders on one RomFS movie); in `runtime/vk/vk_pipeline.c`, `vk_pipeline_prepare_shaders()` (`--prepare-shaders`,
`tools/prepare_shaders.py`), `vk_pipeline_warm_shaders()` (`--warm-shaders`, `tools/build_game.py`),
`vk_pipeline_prepare_pipelines()` (`--prepare-pipelines`, the shared pipeline list), `vk_pipeline_probe_prewarm()`
(`--probe-prewarm`), `vk_pipeline_probe_buildgate()` (`--probe-buildgate`) and `vk_pipeline_spirv_selftest()`
(`--selftest`, the SPIR-V rewrites without a device); `runtime/vk/vk_ffx.cpp` `vk_ffx_probe_cache()`
(`--probe-ffx-cache`).

- `vk_pipeline_crash_note()`: called by `runtime/main.c` `crash_handler()`: a fault while the boot builds pipelines or
  stage libraries leaves a note, which tells the next start that the marker or journal it finds is a crash's and not a
  stopped start's (`runtime/vk/vk_pipeline.c` `pm_skipped()`).
- `vk_driver_cache_env()` (`runtime/vk/vk_device.c`): the drivers' own shader caches beside the exe
  (`MESA_SHADER_CACHE_DIR`, on NVIDIA's Linux driver `__GL_SHADER_DISK_CACHE_PATH` and `_SIZE`) in the environment,
  once; before any thread can load a Vulkan driver.

## `runtime/dispatch.h`

Guest address to host function lookup for indirect branches (`rt_dispatch()`, declared in `runtime/cpu.h`). How lifted
code is laid out and called: "2. CPU: lift ARM64 to C" in [architecture.md](../architecture.md).

One flat table covers every module's code range: slot `(pc - lo) >> 2` holds the host function for a guest function
start, as a 32-bit offset from an anchor address. Lookup is one range check and one load. The table is built once by
`rt_dispatch_init()` and never written again, so threads read it without locks.

- `RtModule` is a lifted module: its place in the guest layout and the generator's sorted function table. `base` and
  `size` are the guest image (`MOD_<NAME>_BASE` / `_SIZE` of the generated `layout.h`); `addrs` is `<mod>_fn_addrs`, the
  guest function starts, strictly ascending; `fns` is `<mod>_fn_ptrs`, the lifted function for each start; `count` is
  `<mod>_fn_count`.
- `RtLayoutEntry` and `rt_linked_modules()`: the module glue that `tools/build_game.py` writes for each build
  (`build/<version>/modules.c`), from `layout.json` and `layout.h`: every module of the layout, and the modules linked
  into this executable.
- `rt_register_host()`: registers a host function at a guest address outside every module (HLE entry points). Allowed
  only before `rt_dispatch_init()`; the registry is fixed afterwards.
- `rt_dispatch_init()`: builds the table from the modules (kept by pointer; they must outlive the process). Call it
  once, before any guest thread starts: thread creation publishes the table to the threads. Fatal on a malformed table:
  unsorted or duplicate starts, a start outside its module, overlapping modules, host code spread over 4 GiB.
- `rt_dispatch_lookup()`: the host function for a guest address, or NULL when nothing is there. Does not run guest code.
- `rt_guest_name()`: "0x7000001234 (main+0x1234)" for a guest address inside a module, else the bare address. Returns
  `buf`.
- `RtDispatchStats`: `lo` and `hi` are the guest range the table covers; `slots` is (hi - lo) / 4; `table_bytes` is what
  is reserved for the table; `functions` the filled slots; `host_fns` the `rt_register_host()` entries.
- `rt_fatal()`: the runtime's fatal error: the message to stderr, then exit. Used by the dispatcher and the stubs. It
  ends with `RT_FATAL_EXIT` (70).
- `rt_fatal_code()`: the same end with an exit code of its own, for a failure that a script or the launcher tells from
  the others by it (`runtime/vk/vk_device.c` `vk_device_lost_check()` ends with `RT_LOST_EXIT`). 71 is the crash
  report's (`runtime/main.c` `crash_handler()`).
- `rt_fatal_note_add()`: lines of context for a fatal error's report, which `rt_fatal()` and the crash report
  (`runtime/main.c` `crash_handler()`) write after their own: `fn` logs one with `rt_log()`. It runs on the failing
  thread, which may hold any lock, so it reads only what needs none. At most four; adding one twice keeps one.
  `rt_fatal_notes()` runs them, once per process.
- `rt_ending_enter()`: the one end of a failed run: the fatal error (`rt_fatal()`) and the crash report
  (`runtime/main.c` `crash_handler()`) call it first, with the code they end the process with. 1 for the first caller,
  which reports and ends the process; 0 for that thread again (a failure inside its own report), which ends at once with
  its code; any other thread waits, logging nothing, until the process ends, and after 30 s ends it with the first
  caller's code.
- `rt_rumble_stop_bounded()`: HD rumble stopped before a failed run ends, on a thread of its own that the caller waits
  for 300 ms at most.

## `runtime/dispatch.c`

Indirect-branch dispatch: guest address to lifted function, O(1).

The layout of the table: one `uint32_t` slot per 4-byte guest instruction over [lo, hi), where lo is the lowest function
start of any module and hi is past the highest. A slot is 0 when no function starts there, else the host function's
address minus `anchor` (an address just below the lowest host function), so 4 bytes are enough as long as all lifted
code lies within 4 GiB, which a single PE image guarantees. Memory: 4 bytes per 4 guest bytes, that is the size of the
guest range covered. The gaps between code ranges (rodata, data, bss, alignment) are reserved but never written, so the
OS never backs them with RAM; resident memory is about the total .text size of the modules.

The table is filled once by `rt_dispatch_init()` and then made read-only, so any number of threads read it without locks
or per-thread caches. Host (HLE) functions at addresses outside the modules live in a small hash table that is also
fixed at init; they are looked up only after the flat table misses.

- `TABLE_SPAN_LIMIT`, 1 << 30: the guest bytes covered by the table, and its size in bytes.
- `HOST_CAP`, 8192: the host function registry, a power of two.
- `D` is the hot data read by every `rt_dispatch()`: one cache line (aligned to 64).
- `g_mods[]` is sorted by base.

### `rt_fatal_notes()`

Once per process: a second fatal error, on another thread or inside a note, adds none.

### Weak stand-ins

Builds without the real files (the unit checks) link these empty versions:

- `fs_save_crash_wait()`: the real one is in `runtime/hle/fs_host.c`; such builds have no saves.
- `perf_summary_flush_try()`: the frame-time summary and the stutter map at a fatal error (`runtime/diag/perf.c`).
- `session_summary_fatal()`: the session summary at a fatal error (`runtime/session_summary.c`).
- `input_rumble_hd_stop()`: HD rumble's neutral value before a failed run ends (`runtime/input/input.c`
  `input_rumble_hd_stop()`); such builds have no controller.

### `rt_rumble_stop_bounded()`

The stop goes through SDL, which takes its own joystick lock: a crash on SDL's thread while it held that lock would hold
the ending thread there for good, and the process would never end. So it runs on a thread of its own, and the ending
thread waits for it at most 300 ms and never joins it. A controller left buzzing is the lesser harm.

### `rt_ending_enter()`

One end for a failed run. The platform layer calls the crash handler on every thread that faults, without waiting for
the others (`runtime/platform/win32.c` `crash_veh()`, `crash_filter()`), and a fatal error can come beside a crash from
the same bad state (an allocation that failed here, a null pointer read there). The later end cut the first report where
it was, together with its wait for a save Commit and RivaTuner's restore, and its own FATAL line and exit code replaced
the first (the launcher shows the last FATAL). Another thread now waits until the first end is done; it logs nothing,
since it may hold the log queue's lock, which the first report only tries. A first report that hangs still ends: after
30 s the waiting thread ends the process with the first report's own code (a fatal error's 70 or 72, the crash report's
71), so the launcher reads the end as the report says it.

`ending_tid` is the thread that reports, 0 before one does. `ending_code` (71 at first) is the code that report ends
with, stored right after it took `ending_tid`.

### `fatal_end()`

The fatal line, the report's notes and what has to be flushed, then the exit with `code`. In order:

- `rt_ending_enter()`: a fatal error inside this thread's own report gets no second one and terminates.
- `rt_log_ending()`: the report reaches the log file past its cap (`runtime/main.c` `log_sink()`). `rt_log_sync()`: the
  lines queued for the log thread, then these, written here.
- The line's text is formatted once more for the session summary: `rt_vlogf()` uses up `ap`, so a copy of it. The buffer
  (1024 bytes) is static: only the thread `rt_ending_enter()` let through gets here.
- The FATAL line itself stays as the launcher reads it; the time goes on a line of its own.
- The memory numbers: when an allocation fails, they help distinguish high commit in this process from system-wide
  pressure.
- `fs_save_crash_wait()`: a save the game already reported as done finishes first (bounded).
- `prof_sampler_write()`: `NX2SARIA_PROFILE`: what was sampled until now, else a crash loses the whole profile.
- `rtss_limit_crash()`: RivaTuner Statistics Server's profile goes back at the next start, from the line the run keeps
  for it in `rtss-profile.txt` (`runtime/pacing/rtss_limit.c` `rtss_limit_crash()`, as in `runtime/main.c`
  `crash_handler()`), with no call into RTSS's library here; only where that line could not be written is it put back
  now, and never from RTSS's own thread.
- `rt_rumble_stop_bounded()`: a controller on HD rumble: nothing renews or stops its vibration after the end.
- `perf_summary_flush_try()`: "frametime: at a fatal error" and the stutter map, the session's last period: after what a
  kill of the process must not lose, as in the orderly end (`runtime/gfx/gfx_present.c` `rt_exit_steps()`); `ft_stop()`
  waits up to 50 ms.
- `session_summary_fatal()`: `logs/session-summary.json` for the launcher, from the session's numbers the line above
  closed; static buffers only.
- `plat_terminate()`, not `_Exit`: in the C runtime that is ExitProcess, which stops the other threads wherever they are
  and then runs every library's detach on this thread. A driver's detach that faults there turned a fatal error's end
  into a crash report with another code (`runtime/platform/win32.c` `plat_exit()` names one such detach), and one that
  hangs held the process.

### `host_hash()`

A multiplicative hash (0x9E3779B97F4A7C15) that keeps 13 bits.

### `table_alloc()`

`plat_mem_alloc()` gives zero pages, backed on first write.

### `rt_dispatch_init()`

The modules are sorted by base and checked for overlap. The function tables are checked: sorted, aligned, inside their
module; the same pass finds the range of guest starts and of host functions.

### `rt_dispatch_lookup()`

The index is `pc - lo` rotated right by 2, so a misaligned pc lands far above the slot count.

### `dispatch_slow()`

Host functions, and the fatal error. A separate tail call keeps the fast path free of a stack frame.

### `dispatch_preempt()`

A pending preempt request at an indirect call (a safe point: the guest state is all in `Cpu`). Out of line, so
`rt_dispatch()` keeps no stack frame.

### `rt_dispatch()`

Jumps to the guest address in `c->pc`. Always a tail call, so chains of indirect jumps keep the host stack flat. A
pending preempt request costs two loads and a branch to check.

## `runtime/guest_setjmp.c`

setjmp / longjmp for lifted code.

The guest's longjmp restores x19-x30, sp and d8-d15 and returns to the setjmp call's return address, which in lifted
code is the middle of a host function: `rt_dispatch()` has no function there and the run ends ("not a function start").
The game's font rasterizer reaches it whenever a large glyph's cells overflow its pool: main+0x166215c longjmps back to
main+0x1661670, which reports the overflow, and the caller renders the glyph again in smaller bands. Text at a 2160-high
picture does it in normal play.

The overrides keep both halves on the host:

- `setjmp(env)`: the guest registers longjmp restores, kept for `env`, and the host state of the lifted function that
  made the call as it will be when that call returns (`runtime/platform/platform.h` `plat_frame_capture()`: the host
  stack unwound up to the frame running the guest function that contains the call), so a jump can make the call return
  again. Returns 0.
- `longjmp(env, val)`: the guest registers back, x0 = val (1 for 0), and that host state resumed: the lifted caller
  continues after its setjmp call, reads x0 from the `Cpu` and takes the "returned again" path.

Host locals the caller keeps in callee-saved registers come back as they were at setjmp, the ones in its stack frame as
last written; C allows only the unmodified ones to be relied on after a longjmp, and the guest code was compiled under
the same rule. A longjmp to a buffer this file did not fill (or after the setjmp's function returned) is fatal.

- `MAX_MODULES`, 16, as in `runtime/dispatch.c`.
- `JmpEnt`: `env` is the guest jmp_buf address, 0 free; `thread` the `Cpu.thread` of the setjmp; `x[12]` and `sp` are
  x19-x30 and sp; `d[8]` is d8-d15 (the low halves are what the ABI keeps); `seq` is `jt_seq` at its last setjmp: the
  smallest is the entry filled longest ago. `jt_seq` is under `jt_lock`.
- `Rev`: the host function starts of the lifted code, sorted, with their guest starts: the guest function a host address
  runs.

### `guest_of_host()`

The guest start of the lifted function whose host code contains `rip`; 0 outside (or above the last start).

### `guest_function_of()`

The guest start of the lifted function containing guest address `a`, 0 for none.

### `slot_of()`

The hash is a multiplicative one (0x9E3779B97F4A7C15) that keeps 10 bits, for the 1024 slots of `JMP_N`.

### `ov_setjmp()`

The BL to setjmp sits just before the return address, so the caller is the function containing x30 - 4.

When every slot holds a buffer (entries are never removed, and a long session keeps meeting new buffer addresses), the
one filled longest ago gives its slot. With no free slot left a lookup walks the whole table, so the new buffer is found
wherever it sits. A longjmp to the replaced buffer ends the process as one to an unknown buffer does (`ov_longjmp()`);
before, the 1025th buffer itself ended it.

### `ov_longjmp()`

The entry is copied into a `_Thread_local` static: the frame is resumed from there, not from the stack being abandoned.
On a failure the log names the guest frames that led here: from the setjmp's caller, its return address, then the
frame-pointer chain of x29 as setjmp saw it.

### `setjmp_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): the jmp_buf table's entries. Entries are never removed; a
full table replaces the entry filled longest ago (`ov_setjmp()`).

## `runtime/memory.c`

The guest address space and the memory SVCs: the heap, the modules' data permissions, stack moves, QueryMemory, physical
memory in the alias region, the memory ids of GetInfo, and the kernel-region allocator. On top of that it holds three
things the Vulkan back end leans on: frozen regions, the write-back probe and the lazy write-back. The file's first two
lines are a licence notice and stay in the code.

Every region is a plain reservation; pages are committed and decommitted at 4 KiB granularity. Decommitted pages come
back zeroed, so no stale-page bookkeeping is needed. The stack and heap block and the alias region are reserved
write-watched (`runtime/platform/platform.h` `plat_mem_reserve()`): `mem_written_pages()` reports the pages the CPU
wrote, which the Vulkan back end uploads again.

### Constants and state

- `HIGH_SEARCH_FROM`, 0x1000000000 (64 GiB): where the stack and heap block goes with the tracker API on.
- `LOW_SIZE`, 14 GiB: the stack and heap block, with room for a Switch 2 heap (`HEAP_CAP`).
- `HEAP_CAP`, 12 GiB: the heap region. A Switch 2 application pool is 9809 MiB (the 12 GB console arrangement).
- `DEFAULT_POOL` is `rt_app_pool_bytes`, the retail application pool of the title, a generated value: 0xCD500000 for the
  Switch 1 title, 9809 MiB for a Switch 2 one.
- The `ST_*`, permission and attribute values and the `R_*` results are Horizon's memory states, permissions, attributes
  and results. `R_INVALID_ADDRESS` (0xcc01) is a misaligned address; `R_INVALID_STATE` (0xd401) is InvalidCurrentMemory.
- `mods_lo` and `mods_hi` are the fixed module window.
- `heap_lazy` is `NX2SARIA_HEAP_LAZY` (Windows): the heap is committed page run by page run on first use.
  `heap_lazy_faults` and `heap_lazy_bytes` are the accesses that committed heap pages and the bytes they committed;
  `heap_lazy_ns` and `heap_lazy_max_ns` the time in those faults, lock wait included, since the last report.
- `alias_pages` holds 1 per mapped 4 KiB page of the alias region.
- `module_perm[]` holds, per data page of each module, the permission or-ed with `ATTR_PERMISSION_LOCKED`.

### Frozen regions

A 2 MiB region of the watched reservations that nothing wrote for a while can be made read-only (`mem_freeze()`), so the
back end's write-watch queries skip it: no page of a frozen region changes before a thaw. The first write faults and
`thaw_fault()` makes the region writable again before the write retries. A kernel write into guest memory (a file read)
fails on a read-only page instead of faulting, so file reads thaw their buffer first (`mem_thaw()`), and so does every
commit, decommit and protection change in this file. A region stays thawed for `FRZ_HOLD_MS` (5000) after a
`mem_thaw()`: a file read still in flight writes its buffer later.

Nothing writes guest memory under `frz_lock`. It is not a leaf: `lz_lock` and `vm_lock` come inside it, a strict thaw's
page calls run under it (`thaw_pages_vm()`), and a holder may wait for a range of the lazy write-back in flight, whose
fetch (`runtime/vk/vk_memory.c` `lazy_fetch()`) takes the queue lock and waits for the GPU (see [Ranges in
flight](#ranges-in-flight); in a thaw under the lock: a mapping change, the lazy heap's commit or a write fault
`lazy_fault()` did not take; and with `NX2SARIA_WB_LAZY_UNLOCKED=0` the fetch itself runs under `frz_lock`). So no
holder of the queue lock may touch guest memory or take `frz_lock`.

- `thaw_ms[]` (under `frz_lock`): the GetTickCount of the region's last thaw request, 0 never.
- `frz_ms[]`: GetTickCount | 1 of its last freeze, 0 never. `frz_hot_ms[]`: its hot time now, 0 not hot.
  `frz_hot_until[]`: no freeze before this GetTickCount, 0 none. `frz_hot_holds`: the quick thaws that held a region,
  since the last stats. All under `frz_lock`.
- `frz_count[4]`: freezes; thaws by a write fault, by `mem_thaw()`, by a mapping change.

**Hot regions.** A region a write thaws soon after its freeze is one the game keeps writing (the buffers its loaders
stream an area into): frozen again as soon as it was idle a moment, it was thawed again moments later. Running through
Hyrule Field froze and thawed about 8,000 regions in 6,000 frames, and while the loaders wrote and the driver locked new
memory each thaw's VirtualQuery took 15 to 20 ms: a loader thread stalled 78 to 111 ms in thaws in each of the hitches
of 125 to 140 ms of that walk, with the game's main thread waiting for it. So a write that thaws a region within
`FRZ_QUICK_MS` (60000) of its freeze holds the region thawed for its hot time, `FRZ_HOT_MIN_MS` at first and doubled at
each such thaw up to `FRZ_HOT_MAX_MS` (600000); a region that stayed frozen longer starts again from nothing. A freeze
only saves later write-watch queries over the region, which a region written that often needs anyway.

**Thaws outside `frz_lock`** (`NX2SARIA_FREEZE_ASYNC`, default 1; 0: every thaw under the lock, as before; `frz_async`,
read in `mem_init()`). While the game streams an area it creates and finalizes up to 1,500 memory pools a second, the
driver pins and unpins the imported ones, and the VirtualQuery and VirtualProtect calls of a thaw then take 10-20 ms
each. Under `frz_lock` that held the replay (`mem_lazy_skip()`, the freeze pass) and every other thread that faulted or
thawed, a 25-45 ms frame. So a thaw by a write fault or by `mem_thaw()` marks its regions `FRZ_THAWING` (2) under the
lock, makes their pages read-write without it and ends them under it again. A region being thawed is no longer frozen
for anyone who asks (a freeze, an arm, a probe leave it alone); a fault in it waits for the thaw to end and runs again;
a mapping change waits for it first (`thaw_wait_locked()`), so a late VirtualProtect never undoes the change;
`mem_thaw()` makes its pages read-write itself. `freeze_locked()` keeps the runs it made read-only (`frz_runs`), so a
thaw needs no VirtualQuery.

- `FrzRuns`: up to 16 runs of pages of the region, each [first, end). `frz_runs[]` (under `frz_lock`, written at a
  freeze): NULL or `n` 0 means not kept.
- `frz_async_n`: thaws done outside the lock, since the start. `frz_protect_fails`: the VirtualProtect failures of those
  thaws, since the start.

**`vm_lock`** is a leaf lock held around every write-watch query (`written_pages()`) and around the page calls of a
freeze and a thaw: on Windows each kind takes the process's address-space lock, and with both running at once a game
thread's thaw waited about 16 ms per page call behind the replay's queries, 100-300 ms for a thaw of several runs (a
frame of that length about every second standing in Hyrule Field on an RX 580). Taken in turns here, a thaw waits for
one query at most.

### Probed regions

`NX2SARIA_WB_PROBE=1`, a diagnosis. The back end copies what the GPU wrote into a mirrored pool back into guest memory
after every submission, in case the CPU or a host path reads it; whether anything does is what the probe finds out. A 2
MiB region that lies wholly inside a range just written back is made inaccessible (`mem_probe()`). Its first read or
write faults: `probe_fault()` gives the region back the protection it had (read-only when frozen, else read-write),
notes the address, the instruction and the thread, and the access runs again. The bytes are in guest memory all along,
so the probe only watches. A probed region is not frozen anew (`freeze_locked()` finds pages that are not read-write),
and every thaw, which precedes file reads into guest memory and mapping changes, ends the probe of its region first.

`prb_hit[]` (`PRB_HITS`, 64; under `frz_lock`) holds the first accesses of each report; `prb_nhit` goes with it;
`prb_arms` counts the regions armed and `prb_ends[2]` the probes ended by a read and by a write.

### Lazy write-back

`NX2SARIA_WB_LAZY`; unset: on under Windows except on AMD's own driver, off elsewhere, as `runtime/wb_lazy_choice.h`
decides and gives the reasons for. The back end's side is the "lazy write-back" part of `runtime/vk/vk_memory.c`.

It was off by default in 0.7.0: with it on, single frames came out with the whole scene's shadowing missing, 10 to 180
of 1,800 presents standing in Hyrule Field with other programs keeping the CPU busy and none found with it off. That was
not this code's doing: frames were drawn from copies taken before the game's signal that their data was written (the
"events the CPU signals" part of `runtime/vk/vk_exec_xfer.c`), and with the write-backs gone the frames came faster and
the signal fell into the replay more often. With that fixed, two such runs under eight busy threads showed none.

What a shader wrote into a mirrored pool is copied back into guest memory at the end of every submission, in case the
CPU reads it. The game's CPU does not read most of it: standing in Hyrule Field, 66 MiB a frame went back over PCIe, 7
to 9 ms of each frame's 13 to 15 ms of GPU time, and the write-back probe found no access to it in 40 s. So a range that
went back once is armed here: its whole pages are made inaccessible (`mem_lazy_arm()`), and while it stays armed the
back end leaves its later writes in the mirror (`mem_lazy_skip()` marks it stale). The first read or write of an armed
page faults: `lazy_fault()` has the back end copy the range's bytes down now (`lz_fetch`, a GPU copy after everything
submitted so far), gives the pages back and runs the access again, and the range stays out for a while (`lz_hot`: 1 s,
doubling to 32 s while it keeps being touched), written back every submission as before; by default the whole range goes
that way, and with `NX2SARIA_WB_LAZY_CHUNK` on only the piece around the address does and the rest of the range stays
armed (see [Pieces](#pieces)). Every thaw (before the kernel writes or reads guest memory, and before mapping changes)
releases the armed ranges of its regions the same way.

A region with armed pages is not frozen (`freeze_locked()` wants every page read-write), and a frozen one is not armed.

A fetch brings what the GPU work handed over so far wrote. The part of a submission that is being recorded has left its
writes out as well, and makes up for it when it finds a range it skipped no longer armed (`runtime/vk/vk_memory.c`
`lazy_part_check()`). Between that check and the part's hand-over a release would be seen by neither side, so the replay
opens a window there (`mem_lazy_window()`) and a fault on an armed page waits for it to close: the fetch then has the
part's writes. Only a fault of the replay thread itself, a wait of 20 ms, or a release by a thaw goes ahead inside the
window (`lz_gaps` counts the faults): that part's writes to the range then reach guest memory with the range's next
write-back.

The list of armed segments has a lock of its own, `lz_lock`, held only for a few steps over the list and never across a
system call or a fetch: the replay asks it at every draw that writes a mirrored pool (`mem_lazy_skip()`) and at the end
of every part (`mem_lazy_armed()`, `mem_lazy_window()`). Under `frz_lock` it waited for freezes, thaws and arms on other
threads, whose page queries and protection changes take that lock for tens to hundreds of microseconds: 8% of the replay
thread's samples standing in Hyrule Field on an RX 580 (0.7.1, lazy write-back on by default). A change of the list
holds both locks (`frz_lock` first); a read holds either. `lz_lock` also guards the ranges in flight; a wait for one
waits on its condition, `lz_fly_cv`, which releases `lz_lock` meanwhile. `lz_lock` is taken inside `frz_lock`, never the
other way round.

State:

- `LzSeg`: the pages [`lo`, `hi`) of one armed segment; the buffer offset of an address is the address plus `delta`.
- `lzs`, `lz_n`, `lz_cap`: the list, changed under `frz_lock` and `lz_lock`, read under either.
- `lzr[]`: a segment reaches into the region. `lz_hot[]` (under `frz_lock`): `plat_now_ms() | 1` until which nothing is
  armed in the region. `lz_back[]` (under `frz_lock`): the seconds of the region's last hold.
- `lz_want` and `lz_chunk` are the settings, read in `mem_init()`.
- `lz_clean` is the back end's test that the mirror holds every CPU write to [lo, hi) (`runtime/vk/vk_memory.c`
  `lazy_clean()`): a fetch copies the mirror's bytes over the whole range, so a CPU write the mirror does not have yet
  would be lost to it. `lz_unclean` (under `frz_lock`): the arms refused since the last report because the mirror lacked
  a CPU write.
- `lz_no[]` (under `frz_lock`): the bytes `mem_lazy_arm()` left unarmed since the last report, by reason (`LZ_NO_*`):
  the region is frozen, held after a touch (`lz_hot`), held after a thaw, has pages that are not read-write (or lacks a
  CPU write in the mirror), or the bytes lie in the partial pages at a range's ends. They are written back at every
  submission that writes them. `lz_no_small` counts the ranges without a whole page and `lz_arm_calls` the ranges
  written back.
- `lz_notes` (`LZ_NOTES`, 8; under `frz_lock`): the first faults on armed pages since the last report, for its lines
  (trace perf).
- `lz_faults[2]`, `lz_other`, `lz_gaps`, `lz_lock_wait_max_ns` (under `frz_lock`, since the last report): faults by a
  read, by a write, other releases, faults inside a part's window, and the longest wait of a fault for `frz_lock`
  (behind other threads' thaws, freezes and arms, and their fetches where those run under it).
- `lz_fetches`, `lz_fetch_bytes`, `lz_fetch_ns`, `lz_fetch_max_ns`, `lz_joins`, `lz_join_max_ns` (under `lz_lock`, since
  the last report): segments fetched, their bytes, the time in fetches and the longest fetch (one call of `lz_fetch`,
  which may carry several segments); touches that waited for a range in flight and the longest such wait; and the most
  ranges in flight at once.
- `lz_win_tid` (under `lz_lock`): the replay thread's id while its part is between check and hand-over.
- `lz_fetch_total` and `lz_fetch_ms` are for a fatal error's summary line (`mem_lazy_note()`), read without a lock,
  since the thread that fails may be the one inside a fetch: the segments fetched since the start, and `plat_now_ms() |
  1` at the end of the last fetch (0: none yet). Those in progress are in `lz_fly`.
- `lz_pieces`, `lz_piece_bytes`, `lz_heavy` (under `frz_lock`, since the last report); `lz_foreign` (under `frz_lock`,
  since the start): segments of another buffer `mem_lazy_skip()` gave back.

### Pieces

`NX2SARIA_WB_LAZY_CHUNK` (default 0: a touch releases its whole range and holds its regions). It is off because with it
on the graphics device was lost about 2 s after a save load in Hyrule Field at night, in 7 of about 22 loads on an RTX
5090, a GPU read fault at the start of a large buffer, and in 0 of 15 with it off; why is not known yet.

With it on, the first touch of an armed range gives back only the `LZ_CHUNK` bytes around the address (`LZ_CHUNK_SHIFT`
16), fetched alone, and only that piece is held (`lz_hots`, 1 s doubling to 32 s like a region's hold); the rest of the
range stays armed. More than `LZ_HEAVY_N` (4) touches in one region within `LZ_HEAVY_MS` are a reader going through the
range: its range goes whole and its regions are held, as before, and while the region is touched again within
`LZ_HEAVY_KEEP_MS` its touches go that way at once, so such a reader pays `LZ_HEAVY_N` faults more, once.

The game's CPU reads a few words at the start of buffers its shaders fill (12 bytes into ranges of 6.3 and 15.2 MiB,
among others), about twice a second in a village and in Hyrule Field. Each read gave back the whole range and held every
2 MiB region it reached for up to 32 s, in which nothing was armed: 22 to 26 MiB a frame went back to guest memory at
the end of every submission although nothing read them, inside the GPU work the game's fence waits for. With the pieces
1.7 MiB a frame go back, on 17 to 18 faults in 1,000 frames where there were 14 to 16 and with the same time spent
fetching (4 to 5 ms in 1,000 frames), and the village ran at 135 to 138 frames a second where it had run at 122 to 124.

- `LZ_HOTS`, 1024, a power of two. `LzHot`: `key` is the chunk number + 1, 0 free; `until` as `lz_hot`. `lz_hots[]`
  (under `frz_lock`) is open addressing with a short linear probe; `lz_nhots` counts the entries ever used (0: no lookup
  needed).
- `lz_touch_ms[]`: `plat_now_ms() | 1` of the first piece of the region's run; `lz_touch_n[]`: the pieces given back in
  that run; `lz_heavy_ms[]`: the same stamp of its last touch taken as a reader's. All under `frz_lock`.
- `LzPiece`: the segment the piece came from, and the piece [`c0`, `c1`).

A fault at an address in a segment: the `LZ_CHUNK` piece around it goes into flight and is held
(`lazy_piece_hold_locked()`); what is left of the segment on either side stays armed. Its fetch and page calls run in
flight, as a whole segment's do, without `lz_lock`, which the replay asks at every draw that writes a mirrored pool and
at the end of every part.

### Ranges in flight

A segment leaves the armed list with its pages still inaccessible; then its bytes come down when the mirror's are newer
(`lz_fetch`: a GPU copy after everything handed over, and a wait for it) and its pages get their access back
(`lazy_fly_run()`), without `frz_lock`. Under `frz_lock` a fetch held up every other fault, thaw, freeze and arm, the
replay's copies of guest memory (`mem_copy_readable()`) and the completion thread's arms for the length of the copy; and
a write that lost the race to it released every other armed range of its 2 MiB region (`thaw_fault()`). From the moment
it leaves the list until its pages are read-write again the range is an entry of `lz_fly`:

- a fault on it waits for that entry alone, on `lz_fly_cv` and never on `frz_lock`, and runs the access again
  (`lazy_fly_join_locked()`);
- a thaw or a release that reaches it waits for it (`lazy_drop_region_locked()`, `mem_lazy_release()`), so the kernel's
  access, a mapping change or the end of the buffer comes after its copy and its page calls;
- an arm leaves its pages alone (`mem_lazy_arm()`), a freeze its region (`lzr`), and `mem_lazy_touches()` counts it.

Every segment that leaves the list with inaccessible pages becomes an entry in the same `lz_lock` hold, and an entry
ends only once its pages are read-write; lists and entries never overlap. So a fault that finds its address in neither,
holding `frz_lock` (no arm runs then), has an address that is not ours or no longer inaccessible: the page's state,
asked under that lock, decides whether the access runs again (`lazy_fault()`), and an arm after it only has the access
fault here once more. An entry starts under `frz_lock` and `lz_lock` and ends under `lz_lock` alone, so a holder of
`frz_lock` may wait for one: its fetch takes the queue lock, and no holder of the queue lock takes `frz_lock`. Every
store to `lzr` is made under `lz_lock`, from the list, the entries and the region an arm is protecting (`lz_arm_r`).
Fetches of different ranges run at once (the back end has a slot for each, `runtime/vk/vk_memory.c` `lazy_fetch()`).

`NX2SARIA_WB_LAZY_UNLOCKED=0` (`lz_unlocked`, read in `mem_init()`) brings back 0.10.1's locking: `frz_lock` is kept
from the moment a fault or a release takes a range out until its entry ends, a release takes one segment at a time,
`mem_thaw()` releases under the lock, and `runtime/vk/vk_memory.c` `mirror_evict()` and `hot_check()` release their
victims under `pool_lock`, so one fetch runs at a time. The entries stay with it: a touch of a range in flight waits for
that entry and runs again, the hold is set at the take-out, and `mem_lazy_touches()` counts the range.

- `LZ_FLY`, 64: more than the threads that touch armed pages at once; when full, a touch waits for room.
- `LZ_BATCH`, 16: the segments a release takes out together and fetches in one call (1 with the setting at 0).
- `LzFly`: `bytes` is what to fetch, 0 none. `lz_fly[]` is under `lz_lock`; `lz_nfly`, the entries in use, also (read
  without it as a hint); `lz_fly_cv` (with `lz_lock`) signals that an entry ended; `lz_arm_r` (under `lz_lock`) is the
  region whose pages `mem_lazy_arm()` is protecting, -1 none.

### `unprobe_locked()`

`frz_lock` held exclusively: the pages of a probed region get back what they had, read-only in a frozen region. Every
inaccessible run is the probe's: `mem_probe()` took only regions without one.

### `lazy_flags_locked()`

`lz_lock` held: sets `lzr` of the regions [lo, hi) reaches, from the armed segments, the ranges in flight and the arm in
progress.

### `lazy_fly_find_locked()`

`lz_lock` held: an entry in flight that meets [lo, hi), of buffer `h0` (0: any), -1 none.

### `lazy_fly_wait_entry_locked()`

`lz_lock` held (`frz_lock` too, or not): returns once entry `k` has ended, with the time that took. The timeout only
looks again; an entry ends on every path of `lazy_fly_run()`.

### `lazy_fly_join_locked()`

`lz_lock` held: a fault at `a` waits for the range in flight that holds it. 1 when there was one: the access runs again.

### `lazy_fly_wait_ranges_locked()`

`lz_lock` held: returns once no range in flight meets any of `rg[j]` = {lo, hi, buffer or 0 for any}.

### `lazy_fly_room_locked()`

`lz_lock` held: returns once an entry is free, having released `lz_lock` meanwhile if it waited.

### `lazy_fly_start_locked()`

`frz_lock` and `lz_lock` held, an entry free: the segment, out of the list in this hold, is in flight from here on.

### `lazy_fly_run()`

No lock held, except `frz_lock` in three cases: a thaw under the lock (a mapping change, the lazy heap's commit or a
write fault `lazy_fault()` did not take), `mem_lazy_skip()`'s give-back (which fetches nothing), and with
`NX2SARIA_WB_LAZY_UNLOCKED=0`. The `n` segments `s[]` in flight in the entries `k[]` get the mirror's bytes, the stale
ones in one call of `lz_fetch`, then their pages' access, and leave flight. `n` is at most `LZ_BATCH`.

### `lazy_take_locked()`

`frz_lock` and `lz_lock` held: segment `i` leaves the list, with the stale mark `mem_lazy_skip()` may have set until
now. A later `mem_lazy_skip()` no longer finds it and writes its range back as ever.

### `lazy_hold_locked()`

`frz_lock` held: a fault took [lo, hi) out whole, so its regions are held back from arms (`lz_hot`: 1 s, doubling up to
32 s while they are touched again within a minute, 60000 ms, of their last hold). Set as the range goes into flight, so
an arm meanwhile finds the hold.

### `lazy_hot_locked()`

`frz_lock` held. The hold entry of chunk `c`; `make` 1 takes a free slot of the probe for it, or the one whose hold
ended longest ago.

### `lazy_piece_take_locked()`

`frz_lock` held exclusively and `lz_lock` held: the piece leaves the list, the segment's rest on either side goes back.

### `lazy_piece_hold_locked()`

`frz_lock` held: the piece around `a` is held back from arms (`lz_hots`, 1 s doubling up to 32 s like a region's hold,
while it is touched again within a minute, 60000 ms, of its last hold), as it goes into flight.

### `lazy_take_batch_locked()`

`frz_lock` and `lz_lock` held: up to `LZ_BATCH` armed segments (one with `NX2SARIA_WB_LAZY_UNLOCKED=0`, each fetched on
its own as in 0.10.1) that meet any of `rg[j]` = {lo, hi, buffer or 0 for any} leave the list into flight, into `k[]`
and `s[]`. Returns how many; -1 when some meet but no entry is free (wait for one with `lazy_fly_room_locked()`, then
again).

### `lazy_drop_region_locked()`

`frz_lock` held exclusively (a thaw under the lock: a mapping change, a commit of the lazy heap, a write fault
`lazy_fault()` did not take): every segment that reaches into region `r` goes, its fetch under `frz_lock`, since the
caller's page calls follow under it; then the ranges other threads have in flight there end first. Not a fault on an
armed page: no hold. `mem_thaw()` releases its regions before it takes the lock (`lazy_release_regions()`), so what is
left here is rare.

### `lazy_protect_locked()`

`frz_lock` held exclusively: [x, e), inside one region, becomes inaccessible when every committed page of it is
read-write, or inaccessible as part of a segment already; anything else (a read-only page, a hole, a page the game made
inaccessible itself) leaves it as it is. Then the back end checks that the mirror `ctx` belongs to holds every CPU write
to [x, e) (`lz_clean`): after the protection no write can come in, so a write that came before it is in the write-watch
or the page generations. When the mirror lacks one, the pages are given back and nothing is armed: the CPU wrote into
the range after its write-back reached guest memory and before this arm, and the next sync of the mirror carries that
write up; a fetch before it would have copied the mirror's older bytes over it (single frames with another frame's
lighting constants, or a black scene).

### `mem_lazy_arm()`

Arms the whole pages of [a, a + n), which the back end has just written back from the buffer `h0` into `h1` at offset
`off`: they fault on their next access. Returns the bytes armed now. Not in a frozen, probed, held or thaw-held region.

- A held piece stays out: this turn arms what lies before it, the next ones what lies behind.
- The region is marked before its pages become inaccessible. A fault on them from the protect on then finds the mark,
  waits in `lazy_fault()` for `frz_lock`, held here, and finds the segment or its pages given back. Marked only after
  the push, a read in between had `lazy_fault()` return 0 at once, and `access_fault()` passed the read of an
  inaccessible page to the crash handler. `lz_arm_r` keeps the mark while a range in flight in the region ends meanwhile
  (`lazy_flags_locked()`); when the arm is refused the mark is made again from what is there. Pages of a range in flight
  are inaccessible until it ends and are no segment's: such a range is left alone.
- What touches in the same buffer becomes one segment. Once the segment is in the list `lz_arm_r` goes back to -1 and
  the mark stays.

### `lazy_foreign_locked()`

`lz_lock` held: the last armed segment that meets [a, a + n) and was armed for another buffer or offset than `h0` and
`delta`, -1 none (for `mem_lazy_skip()`).

### `mem_lazy_skip()`

The armed parts of [a, a + n), sorted, into `out` (at most `max`); they count as stale from here on: the caller leaves
the bytes it is about to write there in the mirror `h0`, at buffer offset address + `delta`. Returns how many.

A segment there that was armed for another buffer (a mirror evicted since, whose range a write-back armed while the
eviction ran) is given back instead, without a fetch: its buffer may be gone, and since its mirror went every GPU write
went to guest memory, so guest memory holds the bytes. Marked stale, it would have its next touch copy from the freed
buffer, a GPU read fault that loses the device. Such a segment goes back under `frz_lock`, through flight without a
fetch (stale 0: its pages only), with arms held off until the rest is marked, as before `lz_lock` existed; that is rare,
and the replay's per-draw check holds `lz_lock` only. With one to take and every entry in use, it waits for one and
looks again.

### `mem_lazy_touches()`

0 when no page of [a, a + n) can be inaccessible right now (armed by the lazy write-back, in flight, or probed), so the
CPU can read it without a fault; 1 when one may be.

### `mem_lazy_armed()`

Whether [a, a + n) is armed as a whole still.

### `mem_copy_readable()`

Copies each of `n` ranges of guest memory, `src[i]` = {address, bytes}, to `dst[i]` when every page of it can be read
without a fault; `ok[i]` says which were. An armed range, a probed region or a page the game made inaccessible is left
out instead of faulting on the caller's thread (`runtime/vk/vk_memory.c` `mirror_capture()` runs inside a submission,
where a fetch could not be sent). `frz_lock` is held over all of them, so nothing is armed or probed meanwhile; a range
in regions with neither (`lzr`, `prb`: a page becomes inaccessible only under that lock, after its region is marked, and
a mark goes only once its pages are read-write again) is copied without asking the system for its pages' state; a range
in flight gives its pages back meanwhile, which only makes them readable. Returns how many were copied.

With the lazy heap (`NX2SARIA_HEAP_LAZY`) a heap page nothing touched yet is still reserved, and reading it faults into
`heap_lazy_fault()`, which waits for `frz_lock`: held here, by the same thread, so the copy never returned and every
later fault, thaw and freeze waited behind it. So a range that meets the heap (reserved heap pages may lie in it) is
copied run by run as the system reports its pages: the heap's reserved runs read zero, as they would once committed,
without committing them. Pages are committed and decommitted under `frz_lock` only, so the runs copied are those
checked.

### `mem_lazy_release()`

Releases what is armed in each range `rg[j]` = {lo, hi, buffer} (buffer 0: any), and returns once the ranges of those
buffers in flight there have ended as well: before the buffers a fetch would read go away (`runtime/vk/vk_memory.c`
`mirror_evict()`, `hot_check()`), and before the kernel's access in a thaw (`mem_thaw()`). Up to `LZ_BATCH` segments go
out at a time and are fetched in one call, without `frz_lock` (with it, one segment at a time, when
`NX2SARIA_WB_LAZY_UNLOCKED=0`).

### `lazy_release_regions()`

For `mem_thaw()`, before it takes `frz_lock`: what is armed in the 2 MiB regions of [a, a + n) is released here, its
fetches without that lock, and the regions are held from arms first (`thaw_ms`, which `thaw_begin_locked()` sets as
well), so the thaw finds them settled under the lock. Nothing with `NX2SARIA_WB_LAZY_UNLOCKED=0`: the thaw releases them
itself.

### `mem_lazy_enable()`

The back end is ready to fetch: arming starts. 0 when the setting is off (or the probe is on, which watches the same
pages).

### `mem_lazy_window()`

Opens or closes the window of the replay's part, under `lz_lock`: a fault takes its segment out of the list before this,
or sees the window.

### `lazy_fault()`

An access fault at `a`: 1 when `a` was armed or in flight and its pages are read-write again, or when its page allows
the access by now (another thread gave it back): the access runs again. 0 when `a` is none of the lazy write-back's and
its page does not allow the access (a write to a frozen page goes on to `thaw_fault()`).

- A range in flight (another thread fetches it or gives it back): the fault waits for that one alone, without
  `frz_lock`. The same when it went into flight between that look and the look under the locks.
- The window's test and the segment's leaving the list are one step under `lz_lock`.
- Not ours, or given back since the fault: the page's state decides, asked under `frz_lock` alone (no system call under
  `lz_lock`), so that no arm makes the page inaccessible between the answer and the retry. A write to a page a flight
  gave back runs again here instead of going on to `thaw_fault()`, whose thaw would release every armed range of the
  region; and a read is not passed to the crash handler because an arm (after a release that sets no hold,
  `mem_lazy_skip()`'s give-back) made the page inaccessible again once this returned.
- The wait for the replay's window is up to 400 tries, 20 ms: after the hand-over the fetch has that part's writes.
- With every entry in use it waits for one to end, without `frz_lock`, then looks again.
- With `lz_chunk` and no reader, the piece goes into flight and is held and the rest of the segment stays armed;
  otherwise the whole segment goes into flight, and its regions are held.

### `mem_lazy_pick()`

A test (`NX2SARIA_WB_LAZY_TOUCH`): the first addresses of up to `max` armed segments, another set each call, for a
caller that reads them to make the faults a game's access would.

### `mem_lazy_note()`

The lazy write-back's state as a phrase for a fatal error's summary line, "" while nothing is armed by it. No lock: the
count of armed ranges may be a moment old, and a fetch in progress is named with its thread (this thread's first, if it
has one; the scan of `lz_fly` is a hint), which tells a game that ended inside a fetch (the device lost under it, or its
wait run out) from one that ended elsewhere.

### `mem_lazy_report()`

One log line since the last one; `skipped` is the bytes the back end left in the mirror since then.

### `thaw_pages_vm()`

`vm_lock` held. Makes the pages region `r`'s freeze made read-only read-write again: the runs `freeze_locked()` kept,
else every read-only run (`freeze_locked()` left reserved holes, heap pages given back, as they were; every committed
page of a frozen region was read-write at the freeze, and mapping changes thaw or wait before they run, so these are all
ours). `strict`: under `frz_lock`, a failure ends the process; outside it (a thaw in flight, `FRZ_THAWING`) the pages
are what the freeze left, and a failure is only counted.

### `thaw_begin_locked()`

`frz_lock` held exclusively: what a thaw does before the pages (the hold, the lazy write-back's ranges, where armed
ranges go and ranges in flight end first, and a probe). Returns 1 when the region is frozen.

### `thaw_locked()`

`frz_lock` held exclusively. `why`: 1 a write fault, 2 `mem_thaw()` (holds the region thawed), 3 a mapping change in
this file. A region another thread is thawing (`FRZ_THAWING`) gets its pages made read-write here too, and is ended
here.

### `thaw_end_locked()`

`frz_lock` held exclusively: the end of a thaw. Nothing when another thread ended it meanwhile. For a write (`why` 1), a
quick one makes the region hot and a late one ends its hot time.

### `thaw_range_locked()`

`frz_lock` held exclusively.

### `thaw_wait_locked()`

`frz_lock` held exclusively: returns once no region of [a, a + n) is being thawed outside the lock (`FRZ_THAWING`),
releasing the lock while it waits. A mapping change runs after this: the thaw's VirtualProtect then cannot land on the
pages after the change and undo it.

### `heap_lazy_commit_locked()`

The lazy heap (`NX2SARIA_HEAP_LAZY`, Windows only; off by default). Windows charges every committed page to the system's
commit at once, and SetHeapSize asks for the whole application heap up front: in Castle Town after Hyrule Field the heap
held 6.8 GiB of commit of which 3.4 GiB had ever been resident, with 0.45 GB in the page file. With it on, SetHeapSize
only moves the heap's end, and a page of the heap is committed when something first reaches it: an access of the game's
or the runtime's code faults into `access_fault()`, which commits the reserved runs of the 64 KiB around it; a kernel
read or write of a buffer (`mem_thaw()`, `mem_kernel_access()`) and a pointer check (`mem_host_readable()`) commit their
range first, as the kernel and VirtualQuery do not fault. On Linux pages are committed on first touch already.

The function itself: `frz_lock` held exclusively. Commits the reserved pages of [a, a + n) inside the heap; returns the
bytes committed. A page another path committed is left alone: committing it again would make a read-only or inaccessible
page (a freeze, a probe, an armed lazy write-back) read-write behind their backs. The regions it commits into are thawed
first, as for any mapping change, so that no frozen region holds a read-write page.

### `mem_kernel_access()`

Commits the heap pages of [a, a + n) that the lazy heap left reserved; nothing when it is off.

### `mem_untouched_run()`

For a host scan of guest memory (the rewrite scan of `runtime/input/camera_speed.c`): the end of the run of pages at `a`
that the lazy heap left reserved, at most `end`, or 0 when the page at `a` is not such a page. Nothing was ever written
there, so a scan steps over the run; reading it would commit every page of the heap and undo what the setting saves.

### `mem_host_readable()`

`plat_mem_readable()` for a pointer the host is about to read: untouched heap pages of a lazy heap count as readable
(they are committed here, and read zero as they would have).

### `heap_lazy_fault()`

An access fault on a reserved page of the lazy heap: the 64 KiB around it is committed and the access runs again. 0 when
nothing was reserved there, so that a frozen, probed or armed page goes on to its own hook (a page another thread
committed meanwhile is retried by `thaw_fault()` or the end of `access_fault()`, as the heap is watched).

### `mem_thaw()`

- `lazy_release_regions()` first: the lazy write-back's fetches there, without `frz_lock`.
- `frz_lock` is taken also when nothing is frozen: the hold keeps a read's buffer writable.
- With the lazy heap the thaw commits pages, which is a mapping change (so `thaw_wait_locked()` first); the kernel's
  write would fail on a reserved page as well.
- The regions frozen now are marked `FRZ_THAWING` and made read-write without the lock, up to `MANY` (64) at a time;
  those another thread is thawing are made read-write here too, as the caller's kernel access needs every page of them
  now, and that thread ends them.

### `freeze_locked()`

`frz_lock` held exclusively. Makes the committed pages of region `r` read-only when every one of them is read-write;
reserved holes (heap pages given back) stay as they are, and `thaw_locked()` finds the read-only runs again. The runs
are kept for a thaw without VirtualQuery (`thaw_pages()`), per region from its first freeze on.

### `mem_freeze()`

A region is not frozen when it has armed pages or a range in flight (`lzr`), when it is within `FRZ_HOLD_MS` of its last
thaw request (the `| 1` of the stamp may be ahead of now, hence the signed comparison), or while it is hot and held
thawed (`frz_hot_until`).

### `mem_probe()`

Probes the 2 MiB regions that lie wholly inside [a, a + n), the regions [`r0`, `r1`): makes them inaccessible until
their first access. Returns how many it armed. Not a region a file read may still be writing into (the thaw hold), nor
one with a page that is not plainly read-write (read-only when frozen): those are left alone.

### `mem_probing()`

Pages of guest memory may be inaccessible (the probe, or the lazy write-back): a kernel read of them has to thaw first.

### `probe_fault()`

The platform's access-fault hook while the probe is on: the first access of a probed region ends its probe and is noted;
1 runs the access again. A read that faulted in a region another thread gave back meanwhile retries too; a write goes on
to `thaw_fault()`, and any other fault to the crash handler.

### `access_fault()`

The platform's access-fault hook: a reserved page of the lazy heap first, then an armed page of the lazy write-back,
then the probe. A read that faulted on a page another thread gave back meanwhile runs again; a write goes on to
`thaw_fault()`.

### `mem_probe_report()`

One log block for the probe since the last one: how many regions were armed, how many probes a read or a write ended,
and the first accesses by instruction (an address inside the executable as `nx2navi+offset`, for the linker map or
`nm`). Nothing while the probe is off. An access with the same instruction as an earlier one is counted with it.

### `thaw_fault()`

The platform's write-fault hook (`runtime/platform/platform.h` `plat_mem_fault_hook()`): 1 retries the write. The first
write into a frozen region thaws it and retries. A fault in a region another thread thawed meanwhile retries too (with
another thread's thaw in progress, `FRZ_THAWING`, the write runs again once it ended); any other fault goes on to the
crash handler. With `frz_async` the region's pages are made writable outside the lock.

### `commit()`

Mapping changes (`commit()`, `decommit()`, `protect()`) thaw the regions they touch and run under `frz_lock`, so no
freeze sees them half done.

### `reexec_or_fail()`

The process is re-run when a region is taken: Windows places DLLs, heaps and thread stacks at random addresses, and a
fresh process gets a fresh layout. `REEXEC_LIMIT` is 20.

### `mem_init()`

- The fixed window comes first: modules, kernel region, host-function addresses.
- `nx2saria.ini` is read here (`cfg_init()`), once the fixed windows are claimed and before the first setting: the stack
  and heap block depends on `NX2SARIA_TRACKER_API`, the pool size on `NX2SARIA_APP_POOL` and on the resolution settings
  (`runtime/gfx/resolution.c` decides them once, on this first call).
- The stack and heap block, then the alias region above it, are both searched below the module window. With the tracker
  API on (`runtime/diag/tracker_api.c`), the block is searched from 64 GiB first: a map tracker takes a heap pointer
  below that for none ([tracker-api.md](../tracker-api.md)). The game takes its heap wherever the kernel gives it one,
  as on the console.
- `cfg_start()` is the rest of the settings' start, now that the guest's whole address space is claimed: the quality
  preset's probe (`runtime/cfg/hw_detect.c`) loads the Vulkan drivers, whose libraries, heaps and reservations would
  otherwise be free to land where the block and the region above go. It runs before the pool size, which goes by the
  resolution settings a preset sets (`runtime/gfx/resolution.c` `res_heap_extra_bytes()` decides them once, on that
  first call).
- `thaw_fault()` is hooked before the crash handler.
- `NX2SARIA_WB_PROBE` is a diagnosis: off, no access fault reaches `probe_fault()` and nothing is probed.
  `NX2SARIA_WB_LAZY` arms only once the back end wants it and can fetch (`mem_lazy_enable()`).
  `NX2SARIA_WB_LAZY_UNLOCKED` is the fetches outside `frz_lock`. `NX2SARIA_FREEZE_ASYNC` is the thaws outside
  `frz_lock`; unset means on.
- `NX2SARIA_HEAP_LAZY` is set before the game's first SetHeapSize.

### `mem_start_note()`

The start of a game (`runtime/main.c`, before the boot): one line when the machine's free commit is below what the game
takes in play (`runtime/mem_need.h`; `NX2SARIA_MEM_NEED_MB`, 0 no check), so a log that ends in a failed allocation,
dropped textures or a lost device says at its top that the machine was short before the game took anything. Returns 1
when it was. `probe` is `--probe-memneed`, which prints the numbers either way.

### `written_pages()`

A range inside one watched reservation (the stack and heap block, or the alias region) only.

### `mem_map_gen()`

`map_gen` is bumped (lock held exclusively) by every change of what is mapped or readable: the texture pool of
`runtime/vk/vk_exec_draw.c` looks the slots over unreadable storage up again only then (and now and then).

### `mem_heap_extra_max_kib()`

The application pool is the title's own plus `res_heap_extra_bytes()` (`mem_init()`), and the game asks for a heap of
nearly all of it: past `HEAP_CAP` the request is refused, so this is the most that may be added.

### `mem_set_heap_size()`

SVC 0x01, SetHeapSize. `NX2SARIA_TRACE=perf` times the commit or decommit, the wait for the lock included. `heap_size`
is stored before a shrink, so there is no first-use commit past the new end; reserved pages in the decommitted range are
fine.

### `perm_table()`, `module_page_perm()`, `module_data()`

The module data permissions (SVC 0x02 and 0x03) and the module pages for QueryMemory. `perm_table()` and
`module_page_perm()` run under the lock. `module_data()` gives the module whose data segment holds [a, a+n), or -1.

### `mem_set_attribute()`

The only module attribute the SDK sets is the irreversible RELRO lock.

### `stack_move()`

SVC 0x04 and 0x05, MapMemory and UnmapMemory: thread stacks move from the heap or the alias region into the stack region
by copy; the source is inaccessible while moved and gets the contents back on unmap. `alias_all_mapped()` runs under the
lock.

### `alias_map()`

SVC 0x2C and 0x2D, MapPhysicalMemory and UnmapPhysicalMemory. Horizon maps only the still-unmapped pages of the range
and leaves mapped pages and their contents alone; the game's heaps grow by re-mapping ranges they partly own.
`alias_runs()` runs under the lock.

- The SDK asks for byte ranges; a mapping is whole pages, so the size is rounded up.
- `R_LIMIT_REACHED`: the SDK maps LimitReached to its recoverable out-of-memory result.
- The SDK asks for physical memory the pool cannot give and goes on without it, over and over: a session wrote 132 lines
  of the same refusal. The first of each kind (map, unmap) has its line, and so has a refusal with another result than
  the one logged last; the others are counted, and the frame-time summary's "events" line says how many
  (`runtime/log_stall.c`).

### `taken_gap()`

For SVC 0x06, QueryMemory: the region around `a` that no known region covers. Everything else below 2^39 belongs to the
host and is reported as taken (Code, RX), so the SDK never picks it.

### `run_start()`

The first index of the run that ends at `i`.

### `readable_fast()`

`readable_locked()`'s answer for a range inside the heap or inside the alias region, read from the page table: the
texture checks ask it for every pool slot they resolve and every upload (`runtime/vk/vk_texture.c`), and
`query_locked()` finds the whole run of equal pages around the address for it, scanning `alias_pages` both ways; a
replay profile had 2.6% of the replaying thread's samples there. A range that touches a moved stack (whose source and
destination change the answer, `query_locked()`'s clipping) or crosses into another region goes the general way. 1
readable, 0 not, -1 not answered here. `NX2SARIA_READABLE_FAST=0` turns it off, for A/B comparison in the B blocks only.

It follows `query_locked()`'s order: the low region (the heap among it) before the alias region; inside the heap's
current size the range is one Normal read-write region. In the alias region a mapped page is Normal read-write and an
unmapped one Free.

### `mem_readable()`

Whether every byte of [a, a + n) can be read: no free, inaccessible or no-access page (`mem_query()`'s regions). For
host reads of guest memory the guest itself may never make, such as a texture upload of a pool slot no shader samples
(`runtime/vk/vk_exec_draw.c`, texture pools: stale slots point at unmapped memory).

### `mem_read_hold()`

`mem_readable()` for a read that takes a while on another thread than the guest's: with 1 the range stays mapped until
`mem_read_release()`, as unmapping it (UnmapPhysicalMemory, a smaller heap, a moved stack) waits for the lock. A pool
slot's storage the game has just released could otherwise be decommitted halfway through the copy (a texture upload
faulted that way in `runtime/gfx/tex_deswizzle.c` `tex_bl_copy()`). In between, the caller takes no lock of this file
(`mem_query()`, `mem_readable()` and the mapping calls would wait for themselves). 0: not readable, and nothing held.

### `query_locked()`

Moved stacks: the destination is Stack, the source Normal without access; free and normal regions around them are
clipped.

### `mem_info()`

The GetInfo memory ids. Id 17, the system resource in use, is our bookkeeping (`sizeof stack_maps + ALIAS_PAGES`). Id
28, AliasRegionExtraSize, is 0: there is no kernel-owned extension.

### `mem_kernel_free()`

The kernel region: power-of-two blocks, a free list per size, bump allocation with lazy commit. `KCLASSES` is 29: 64 B
to 256 MiB. A block allocated with a larger alignment sits in a larger class; freeing it into the smaller class only
wastes the rest.

### `mem_lazy_heap_report()`

`NX2SARIA_TRACE=perf`: what the lazy heap's first-use faults cost since the last line (`runtime/diag/perf.c`, every perf
window that had some), so that a hitch can be told from them: the time includes the wait for `frz_lock`.

### `mem_guest_ranges()`

The guest's ranges: the regions reserved at start-up and the loaded modules. The span from the low block to
`RT_ADDRESS_LIMIT` (layout `aslr`) is not one of them: the graphics driver maps device memory into its free parts, and
counted as guest it made the guest regions grow by gigabytes with the textures (13.5 to 32.0 GiB in a session of
2026-11-13).

### `mem_guest_census_log()`

The census line: ranges of one name (the modules) are summed.

### `mem_guest_census()`

`NX2SARIA_MEM_CENSUS`: how many bytes of each guest range are committed, so a process that has grown says which region
holds the memory. A region is reserved whole at start-up and committed page by page, so the walk reads each run of pages
of the same state (a `PLAT_PROT_*` state means the pages are committed); the alias window is the largest and costs the
most steps when it is full. Where the OS has a walk of the whole address space (`runtime/diag/prof_win.c`
`prof_commit_census()`) that walk counts these too and this one is not made (`runtime/diag/perf.c` `census_check()`).

### `mem_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): the guest heap and physical maps, the kernel region's bump
and commit, the moved stacks, and the lazily written ranges with their array's room.

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
