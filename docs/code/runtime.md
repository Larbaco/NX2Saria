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

**Three headers still carry their comments.** `runtime/cpu.h`, `runtime/simd.h` and `runtime/simd_fp.h` are inputs of
the lifted code's object cache: every lifted object depends on them, and the build compares each dependency by its
SHA-256 (`tools/make_game.py` `ObjectCache.current()`). Any changed byte in them, a comment or a space included, makes
every player's PC recompile all the lifted objects at the next update, so a change there should be deliberate and
batched. Their notes are on this page under their own headings, and their comments leave the code with the next change
that rebuilds the lifted code anyway.

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
`runtime/gfx/gfx_shader_probe.c` `probe_shader()`; `runtime/gfx/gfx_shader_probe.c` `probe_census_dir()`
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

## `runtime/cpu.h`

The guest CPU state and the helpers used by recompiled code. The file's first line is a licence notice.

**The comments of this header are still in the file.** `runtime/cpu.h`, `runtime/simd.h` and `runtime/simd_fp.h` are
inputs of the lifted code's object cache: every lifted object lists them in its dependency file, and
`tools/make_game.py` `ObjectCache.current()` compares each dependency by its SHA-256. Any changed byte in them, a
comment or a space included, makes every player's PC recompile all the lifted objects at the next update, so a change
there should be deliberate and batched. Their comments leave with the next change that rebuilds the lifted code anyway;
until then the notes below repeat them.

- `Cpu`: `x[32]` is x0-x30 (`x[31]` is unused; `sp` is separate). The offsets: `sp` 256, `pc` 264 (the target for
  indirect dispatch), `nzcv` 272 (flags in bits 31..28), `tpidr` 280, `tpidrro` 288, `excl_addr` 296, `excl_val` 304,
  `thread` 312 (the runtime thread object), `fpcr` 320 (the guest FPCR/FPSR, for the portable FP/SIMD lowering only; the
  asm path uses the host's), `fpsr` 328, `excl_val_hi` 336 (the upper half of a 128-bit exclusive pair, LDXP/LDAXP of
  two X registers), padding up to 512, and `v[32]` at 512.
- The runtime services, defined in the `.c` files of the runtime: `rt_dispatch()` calls the function at `c->pc`;
  `rt_spin_gate()` is a pass of a spin wait loop (`runtime/hle/kernel.c`, the `spin` patches of `recomp/patches.py`);
  `rt_svc()` is the SVC entry.
- Memory: guest addresses are host addresses, so `LD64` and the other load and store macros read and write through
  unaligned, aliasing types.
- The function-entry profiler: `g_fnprof` (defined in `runtime/main.c`) costs one predictable branch per call when off.
- The exclusive monitor is emulated with compare-and-swap (`EXCL`).
- The LSE atomics (ARMv8.1; the A57 lacks them, the SDK carries them behind a CPU feature check): add, clear, xor, set
  and swap map to `__atomic` builtins in the lifter; min/max need a CAS loop (`ATOMIC_MINMAX`). They return the old
  value.
- The flags are packed NZCV in bits 31..28 (`FLAG_N` and the others).
- The last lines include `runtime/simd.h`: the portable FP/SIMD helpers for code lifted with `--simd=c` (the default off
  AArch64; `recomp/gen.py` includes `simd.h` itself in that mode, so AArch64 hosts can build it too).

### `rt_get_fpcr()`

In the first of the two variants the host FP state is not the guest's: code lifted with `--simd=c` keeps FPCR/FPSR in
`Cpu` (`c->fpcr`, zeroed with the `Cpu`), so the host-side reset in thread start-up has nothing to do.

### `rt_fpcr_sync()`

The MXCSR of a host thread running guest code mirrors `c->fpcr`: FTZ|DAZ = FPCR.FZ, RC = nearest (FPCR.RMode takes the
slow paths of `runtime/simd.h`, which switch RC around one operation). The FP helpers rely on it without checking, so it
is set wherever FPCR or the host FP context changes: MSR FPCR (`recomp/lifter.py`), `rt_call()` (thread and fiber entry,
host code calling guest code), fiber resume (`runtime/hle/kernel.c` `fiber_resumed()`; fibers keep their own MXCSR). The
bits: FTZ 0x8000, DAZ 0x40, RC 0x6000 (the mask 0xE040, the value 0x8040 when FPCR bit 24 is set).

### `ldxp128()`, `stxp128()`

An exclusive pair of two X registers: one 16-byte-aligned 128-bit access (cmpxchg16b; x86-64-v2 has CX16). A pair of two
W registers is a 64-bit exclusive and uses `ldx64`/`stx64`.

### `Flags`

The function-local flags (`recomp/flags.py`). Generated functions keep NZCV in a local `Flags F`, never in `c->nzcv`. A
definition of a known kind stores its operands (a - b, a + b, or a logic result in `a`); `fc_<kind>(&F, CC_xx)` computes
a condition from them, so a compare and its branch become one host compare. `F.nz` holds the packed form for definitions
only known packed (CCMP, FCMP, MSR, ADCS) and for readers that several kinds of definition reach. Every helper is always
inlined with a constant condition, so `F` stays in registers and only the condition's own arithmetic remains.

- `FL_SUB`: subtract (SUBS, CMP): conditions as host compares of the operands.
- `FL_ADD`: add (ADDS, CMN) and logic results (ANDS, TST, BICS): conditions from N, Z, C, V.
- `fc_nz()` and `fnz_nz()`: packed flags.
- `FL_ADC`: ADC, SBC, ADCS, SBCS with the carry in from `F`.

## `runtime/simd.h`

The portable C lowering of AArch64 FP / AdvSIMD / crypto / CRC32 (`recomp/simd_c.py` emits calls with constant fields;
everything is always_inline so the size/lane/op switches fold at -O1). The file's first line is a licence notice.

**The comments of this header are still in the file**, for the reason given under `runtime/cpu.h`: it is an input of the
lifted code's object cache, and any changed byte in it, a comment or a space included, makes every player's PC recompile
all the lifted objects at the next update, so a change there should be deliberate and batched.

Semantics follow the ARM pseudocode, not the host: NaN propagation (first signalling NaN quietened, then first quiet
NaN), FPCR.DN default NaN, FPCR.FZ flushing (inputs and pre-rounding-tiny results), FPCR.RMode for arithmetic /
conversions / FRINTX/I, saturating FP to int conversions, fused multiply-add, ARM estimate tables. Not modelled: FPSR
cumulative exception bits and QC, FPCR trap enables, FP16 arithmetic (ARMv8.2). FPCR.AHP is honoured by the
half-precision conversions. Host assumptions: IEEE binary32/64, MXCSR in round-to-nearest outside the slow paths, with
FTZ|DAZ equal to FPCR.FZ (`runtime/cpu.h` `rt_fpcr_sync()` keeps it so); fma must be correctly rounded:
`-march=x86-64-v3` has the FMA instruction, and the v2 build links `runtime/fma.c` for it.

### Registers

`VREG` is the whole V register as a vector type; `VQ` a source with the upper half cleared when Q=0; `VW` a write,
zeroing the upper half when Q=0. Scalar views of lane 0: `VS8` to `VS64`, `VSF32`/`VSF64`.

A write is one 16-byte store, also where the instruction sets a scalar (the bits above it become zero) or a single lane
(the other lanes stay): a V register written as two 8-byte halves or as one lane makes the next 16-byte load of it wait
until those stores have left the store buffer, because the processor forwards a store to a load only when the one store
covers the load. Assigning a zero-extended u128 to `c->v[d]` compiles to the two halves, so the writes go through a
vector type: `VSET` is a scalar of at most 64 bits in the low bits, zero above; `VSET128` all 128 bits of an integer
value; `VINS` lane i replaced by x, the other lanes kept.

### `simd_whole()`

Returns `v` with where it came from hidden from the compiler: a load of a V register, a change of one lane and a store
of the 16 bytes would otherwise be compiled as a store of that lane alone.

### `simd_rm_enter()`, `simd_rm_rz()`, `simd_rm_leave()`, `simd_directed()`

The MXCSR rounding control (bits 13-14, `SIMD_RC_MASK` 0x6000) is set without fesetround's x87 + SSE double write;
ldmxcsr only on change. `simd_rm_enter()` sets MXCSR RC from FPCR.RMode (RN 00, RP 10, RM 01, RZ 11) and returns the
previous MXCSR. `simd_rm_rz()` forces toward-zero (0x6000). `simd_rm_leave()` restores the MXCSR saved by
`simd_rm_enter()`, only if it changed.

The thread's MXCSR mirrors the guest FPCR.FZ with RC = nearest whenever guest code runs (`runtime/cpu.h`
`rt_fpcr_sync()`), so plain host FP gives the ARM result unless FPCR.RMode asks for directed rounding, which the slow
paths handle: `simd_directed()` is that test.

### Precision conversions

FCVT, FCVTL/N, FCVTXN; FZ flushes single/double inputs, never halves.

- `simd_d2s()`: under FZ, a result tiny before rounding (below 0x1p-126) is flushed here, since host FTZ would keep a
  result rounded up to the smallest normal.
- `simd_d2s_odd()`: round to odd (FCVTXN).
- `simd_d2h()`: double (the exact image of the single/double source, NaN payload left-aligned) to half, with FPCR
  rounding.

### Scalar FP instructions

The FP data processing group. The FMADD family (`fp_fma_s()`, `fp_fma_d()`): `neg_a` / `neg_n` negate the addend / the
first factor before the fused op.

### Integer lanes

Complex ops run lane by lane; constant sizes fold to plain loads and stores.

- `lshr_s()`, `lshr_u()`: a right shift by s >= 1 with optional rounding, as an infinite-precision value.
- `pmul64()` is carry-less.
- The `LOP_*` ops: `LOP_SHL`, `LOP_RSHL`, `LOP_QSHL` and `LOP_QRSHL` are register shifts (b is a signed byte), where U
  selects unsigned; the long ops (`LOP_ADDL` and the others) have result lanes of size sz+1; for the narrow ops
  (`LOP_XTN` and the others) sz is the destination lane size and a/b are sz+1; `LOP_SHLLI` is SSHLL/USHLL: extend, shift
  (long). `LOP_U`, 256, is the unsigned variant flag.
- `lop()`: `xa` and `xb` are the extended operands. For the narrowing ops a (and b for xHN) are 2*es wide.
- `sl_gen()`: the generic lane loop: `ne` lanes; result lane i (size `szd`) goes to lane `offd+i` of d (other lanes kept
  when `keep`, else zero); a is n's lane `offn+i` (size `szn`), b is m's lane `offm+i` (size `szm`), or lane `bidx` of m
  when `bidx` >= 0, or the constant `bimm` when m < 0; the accumulator is d's old lane `offd+i`.
- `sl_addlp()`: SADDLP/UADDLP/SADALP/UADALP.
- `sl_across()`: ADDV, SADDLV/UADDLV, SMAXV/UMAXV, SMINV/UMINV (op 0..3; u is unsigned).
- `sl_pmull64()`: PMULL2/PMULL 1Q (64x64 to 128, carry-less).

### FP vector helpers

Lanes of the 64-bit form are computed on a zero upper half.

- `sv_fred_s()`: FMAXV/FMINV/FMAXNMV/FMINNMV (4S): a pairwise tree.
- `vtoint_s()` and its siblings: lane-wise FP conversions.
- `sv_fcvtl()`, `sv_fcvtn()`: FCVTL/FCVTL2 (h to s, s to d), FCVTN/FCVTN2 (s to h, d to s), FCVTXN/FCVTXN2.
- `sv_tbl()`: TBL / TBX with len+1 table registers.

### CRC32, crypto, loads and stores

- `simd_crc()`: CRC32 / CRC32C (bytes = 1, 2, 4, 8).
- Crypto (AES, SHA1, SHA256): `aes_mul()` takes k in {9, 11, 13, 14}; `sv_aes()` op 0 AESE, 1 AESD, 2 AESMC, 3 AESIMC;
  `sv_sha1hash()` op 0 SHA1C, 1 SHA1P, 2 SHA1M; `sv_sha256hash()` is SHA256H (`part1`) or SHA256H2; `sv_sha_misc()` op 0
  SHA1SU0, 1 SHA1SU1, 2 SHA256SU0, 3 SHA256SU1, 4 SHA1H.
- Loads and stores of lanes: `sl_ldm()`, `sl_stm()` are LDn/STn (multiple structures): `selem` interleaved registers,
  `rpt` repeats (LD1 with 2-4 registers). `sl_lds()` is the single structure form: lane `idx` of `selem` registers; LDnR
  replicates into every lane.

## `runtime/simd_fp.h`

The FP helpers for one precision; included twice by `runtime/simd.h` (F = s/float, d/double). It has no include guard.
Scalar entry points take and return `T`, vector ones `VT` (all lanes; callers zero the unused half for Q=0). The file's
first line is a licence notice.

**The comments of this header are still in the file**, for the reason given under `runtime/cpu.h`: it is an input of the
lifted code's object cache, and any changed byte in it, a comment or a space included, makes every player's PC recompile
all the lifted objects at the next update, so a change there should be deliberate and batched.

The names below are the arguments of the `FN` macro, which adds the precision's suffix.

- `MINN` is the smallest normal.
- `FN(flush)`: the asm hides the float origin of `u`: LLVM turns the bit test into an FP class compare otherwise, and
  with MXCSR.DAZ set (FPCR.FZ) the host compare sees a denormal as zero. The same in `FN(vflush)`.
- `FN(fz)`: the FPCR.FZ input flush for helpers that run host FP.
- `FN(nan1)` and its siblings: FPProcessNaN(s): the first signalling NaN (quietened), else the first quiet NaN, else
  (invalid op) the default NaN.
- The FMA flavours. `FMA_S` / `FMA_V`: fast paths, host MXCSR is round-to-nearest there (`simd_directed()` is checked
  first). `FMA_X`: scalar FMA for the slow paths, any host rounding mode. Without hardware FMA (Ivy Bridge, no `-mfma`)
  clang turns `__builtin_elementwise_fma` into a call to the C runtime's fmaf/fma for every lane, which is very slow and
  keeps the thread outside generated code. For float the product of two floats is exact in a double (24 x 24 = 48 bits),
  so the fused op is done in double. A plain `(float)(x * y + z)` rounds twice and can differ by one ulp; rounding the
  double sum to odd first (Boldo-Melquiond) makes the final float rounding exact: TwoSum gives the error e of s = fl(p +
  z), and when e != 0 and s has an even last bit, s is moved one ulp toward the exact value (the odd neighbour). Inf/NaN
  lanes are left alone (e is NaN there, so the ordered compares are false). Double has no wider type: it keeps the C
  runtime's fma.
- `fma_rn_s()`: TwoSum: s + e == p + zd exactly. `fma_rn_v()`: +1 toward the exact value, that is +1 if e and s share a
  sign, else -1.
- `fma_any_s()`: any rounding mode: directed modes are exact with a plain double (same-direction double rounding is
  harmless).
- `FCALC`: the arithmetic itself, for scalars (KK = K) and vectors (KK = VK). MADD: a + b*d (a is the addend).
  RECPS/RSQRTS get their first operand already negated (FPNeg happens before NaN processing).
- `FBAD`: the lanes whose fast result needs fixing: NaN results (a NaN operand or an invalid op), RSQRTS overflow.
- `FN(rsqrts)`: (3 + a*b) / 2 with one rounding (a already negated): it halves a normal operand first, so a product
  beyond the format's range (which the fast path rounds to inf, or to the largest normal when rounding down) is exact.
- `FN(fix)`: the ARM result for a lane flagged by `FBAD` (operands already flushed). For `FOP_SQRT`: NaN in, or
  negative, gives the default NaN.
- `FN(minmax)`: FMAX/FMIN/FMAXNM/FMINNM (operands already flushed).
- `FN(slow)`: the FPCR.FZ / RMode path: flush inputs, round in the FPCR mode, flush tiny results (tininess is detected
  before rounding on ARM: a result rounded up to the smallest normal is recomputed toward zero).
- `FREDO`: the fast-path results that need a second look: `FBAD` lanes, and +-smallest normal. Under FZ the host runs
  with FTZ|DAZ, which flushes denormal inputs and tiny results like ARM, but x86 detects tininess after rounding: an
  exact result just below the smallest normal that rounds up to it is flushed on ARM only. The slow path decides both
  cases with the operands flushed.
- `FOK`: the complement of `FREDO` for every op but RSQRTS, as one compare: |r| ordered and not the smallest normal
  (LLVM folds it to fcmp one, a single ucomiss + jne for scalars and one vcmpps for vectors).
- `FN(op)`: the scalar FP op (a constant op folds the selection). RMode = nearest runs on the host, with or without FZ.
- `FN(vop)`: the vector FP op: native lanes; any lane `FREDO` flags (or FPCR.RMode) reruns all lanes through the scalar
  path.
- Compares: `FN(cmp)` is FCMP's NZCV; `FN(vcmp)` is FCMEQ/FCMGE/FCMGT/FACGE/FACGT (op 0..4).
- Rounding to integral (FRINT*), `FN(rnd)`: rm 0 N, 1 P, 2 M, 3 Z, 4 A; 5 X / 6 I = the FPCR mode (rint is ties-to-even:
  the host stays in round-to-nearest outside the slow paths; there is no roundeven in MinGW's CRT).
- FP to integer (FCVT[NPMZA][SU], fixed point), `FN(toint)`: saturating, NaN gives 0. `ib` is 32 or 64.
- Integer to FP (SCVTF/UCVTF, fixed point), `FN(fromint_slow)`: rounded in the FPCR mode.
- Reciprocal estimates (bit-exact ARMv8.0 pseudocode), `FN(recpe)`: with |x| < 2^(emin - 2) the result overflows (the
  rounding mode is FPCR bits 22-23); under FZ a result that would be tiny (the exponent at or above (1 << EB) - 3) is a
  signed zero; the table entry (`simd_recip_tab[scaled - 256]`) has 8 fraction bits. `FN(rsqrte)`: the result exponent
  is (380 - e) / 2 for float and (3068 - e) / 2 for double, from 3 * ((1 << (EB - 1)) - 1) - 1; the table is
  `simd_rsqrt_tab[scaled - 128]`.

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

## `runtime/cfg.h`

Settings: `NX2SARIA_*` environment variables and `nx2saria.ini`; existing `NX2NAVI_*` variables and `nx2navi.ini` remain
supported. The header is included by most of the runtime, and by `runtime/platform/win32.c` as `../cfg.h`. The code
behind it and its notes: `runtime/cfg/cfg.c` and [cfg.md](cfg.md).

- `cfg_env()`: the environment lookup for a setting name: `NX2SARIA_*` takes precedence over `NX2NAVI_*` even when
  empty. It accepts either prefixed spelling; non-setting names are passed directly to getenv. Used by the early
  platform probes too.
- `cfg_get()`: getenv for a setting: the environment variable when set, else the settings-file key (the name without
  either prefix, any case), else the device profile's default (`runtime/cfg/cfg.c`: the Steam Deck), else the quality
  preset's (`cfg_preset_*`), else NULL. The same string semantics as getenv; the pointer stays valid for the process (a
  value `cfg_set()` replaces is kept). A name without either prefix goes to getenv unchanged. Safe from any thread once
  `cfg_start()` has returned.
- `cfg_init()`: reads the selected settings file once, from `mem_init()` after the fixed windows are claimed and before
  any setting is read. Logs the file, its key count, unknown keys and environment overrides. `cfg_start()` follows it.
- `cfg_start()`: the rest of the settings' start, from `mem_init()` once the whole guest address space is claimed (the
  quality preset's probe loads the Vulkan drivers, whose libraries and allocations must not land in the stack and heap
  block or the alias region): the quality preset in use (the "quality presets" part of `runtime/cfg/cfg.c`) and then the
  restart-only settings' values (`cfg_pending()`). `game`: 1 for the game itself (`runtime/main.c`: no tool mode), which
  probes the GPU (`runtime/cfg/hw_detect.c`) and keeps the state file `preset-state.txt`; 0 for a tool mode, which goes
  by the state file's last probe and writes nothing.
- `cfg_load()`: replaces the loaded keys with those of the file at `path` (tests). 1 when the file was read, 0 when
  missing. Not thread-safe: nothing else may call `cfg_get()` meanwhile.
- `cfg_reload_file()` and `CfgReloadResult`: reads the selected settings file again (Ctrl+F5, `runtime/ui/menu.cpp`) and
  applies what changed since it was loaded, without a restart: a line changed, added or removed (the key then falls back
  to its default). Each line is read as `cfg_load()` reads it (a comment after a value cut) and each new value is
  checked first (`runtime/cfg/cfg.c` `value_ok()`: the check `cfg_load()` logs at start for values outside their rows'
  sets, and rules for the consumers that take more or stop the game on a bad one); a value that does not fit is rejected
  and the current one stays. Kept as they are, and logged: keys the environment sets, and keys changed in the menu and
  not saved yet or held by its draft (`cfg_hold()`). Applied keys are not marked for `cfg_save()` and `cfg_version()` is
  bumped once. Every key is logged ("cfg: reload ..."). Fills `*r` (may be NULL): `changed` counts the keys that now
  read the new value on this game build, `restart` those of them read at start only. Returns 0 when the file cannot be
  read, and in a run that replays a bug capture's settings (`NX2SARIA_CAPTURE_REPLAY`), which reads no player settings
  file. Any thread.
- `cfg_set()`: live settings (the in-game menu). It stores `value` as the settings-file value of `name` (`NX2SARIA_`
  prefixed), marks it for `cfg_save()` and bumps `cfg_version()`. `cfg_get()` returns it unless the environment sets
  `name`. Any thread.
- `cfg_version()`: bumped by every `cfg_set()`. A consumer that keeps a parsed value compares this with the number it
  last saw (one atomic load) and parses again when it changed.
- `cfg_setenv()`: `plat_setenv()` for an `NX2SARIA_` variable the process reads back: it also bumps `cfg_version()`, so
  lookups see the new value.
- The menu's draft (`runtime/ui/option_rows.cpp`): a live setting is previewed with `cfg_set()`, and Discard puts it
  back. `cfg_state()` reads what `cfg_revert()` needs of `name` before the first change: the stored value (NULL when
  none; the pointer stays valid) and the `CFG_*` flags of its entry (`CFG_DIRTY` an unsaved change, `CFG_REMOVED` a line
  to remove at the next save, `CFG_TEST` a test hook's value, `cfg_set_test()`). `cfg_hold(name, 1)` keeps the entry out
  of every `cfg_save()` / `cfg_save_to()` until `cfg_hold(name, 0)` (Apply), so an F10 / F11 / window-close save made
  while the draft is open does not write a previewed value. `cfg_revert()` restores the value and flags `cfg_state()`
  gave, releases the hold and bumps `cfg_version()`. Any thread.
- A test hook's settings (`NX2SARIA_MENU_TEST_SET` of `runtime/ui/menu.cpp`): `cfg_set_test()` sets `name` as
  `cfg_set()` does, and marks the value as the test's, which no `cfg_save()` / `cfg_save_to()` writes, the save at exit
  included ("cfg: menu test values not saved: <n> key(s): ..." in the log), until a `cfg_set()` of the same key makes it
  the player's. `cfg_test_keep()` makes every test value a plain one, which the next save writes (the hook's `SAVE=1`),
  and returns how many it released. Any thread.
- `cfg_from_env()`: 1 when the environment sets `name`: `cfg_set()` cannot change what `cfg_get()` returns (the menu
  greys the setting out).
- `cfg_is_set()`: 1 when the player set `name` (the environment or settings file), 0 when `cfg_get()` gives a device
  profile's default or NULL.
- `cfg_unknown_keys()`: the keys of the file's lines that are no settings of this version, lower-case with ", " between
  them ("" when there are none; a list too long to keep ends early), and how many there are. Such a line does nothing:
  the log says so at start and the settings menu shows it (`runtime/ui/menu.cpp`), so that nobody takes one for the
  cause of what they see.
- `cfg_device_profile()`: the device profile in use, "deck" or "desktop" (`NX2SARIA_DEVICE_PROFILE`, else what the
  machine is).
- `cfg_save()`: writes the keys `cfg_set()` changed since the last save into the selected settings file beside the game
  executable (`cfg_save_to()`: the file at `path`). Only those lines change: the last line of a changed key gets the new
  value, a missing key goes under its section (after the commented example line of the key when the section has one; the
  section is added at the end of the file when missing), comments and every other line stay byte for byte. 1 on success
  or with nothing to write.
- `cfg_reset()`: the menu's "Reset all settings to defaults". Removes the active line of each key (names without
  `NX2SARIA_`, any case) from the selected settings file at the next `cfg_save()` and its value from memory, so the
  consumer's own default applies again; the commented example lines stay. Keys the environment sets are skipped. Bumps
  `cfg_version()`. Returns how many it reset.
- `cfg_last_save_ok()`: 0 when the last `cfg_save()` / `cfg_save_to()` failed to write (the values stay set in memory,
  the next save tries again).
- `cfg_each_active()`: every setting with a value, for a bug report ("Copy system info"): the settings file and
  `cfg_set()` keys (name without either prefix, upper case; `from_env` 0), then the known `NX2SARIA_` variables the
  environment sets (`from_env` 1).
- `CfgMeta`: what the menu knows about a setting. `key`: without `NX2SARIA_`, upper case. `def`: what the consumer uses
  when the key is unset ("" means no fixed default, the consumer picks per machine). `values`: "0|1",
  "mailbox|fifo|immediate", a range "0..300", or NULL for free text. `live`: applies at once; the others are read at
  start.
- `cfg_supported()`: whether `key` (prefix and case ignored) does anything on the running game build
  (`runtime/cfg/cfg_support.c`): 0 for a setting whose patch set the build lacks or that only another game reads. The
  menu hides such rows; 1 for every other key. Call it after `rt_patches_init()`.
- `cfg_has_section()`: 1 when `key` has a settings-file section of its own in `runtime/cfg/cfg.c` (`cfg_save()` writes
  any other key under [misc]).
- `cfg_log_unsupported()`: logs "cfg: KEY has no effect on this game version" for each key the settings file or
  environment sets that is not supported.
- `cfg_meta()`: the row of `key` (prefix and case ignored); NULL for a key the menu does not show. `cfg_meta_at()`: the
  i-th row (0-based, table order); NULL past the last.
- `cfg_default()`: what `key` reads when the player has not set it: the device profile's value (the Steam Deck's), else
  the quality preset's in use, else the row's default; NULL for a key without a row. The menu's "default" mark follows
  it.
- `cfg_pending()`: a restart-only setting (`live` 0) whose value differs from the one `cfg_start()` kept at start, which
  the running game still uses: 1 while a restart would change it, 0 otherwise and for a live key or one without a row
  (the menu's "after restart" note). `cfg_pending_count()`: how many restart-only settings differ. `cfg_pending_for()`:
  `cfg_pending()` for `value` instead of the key's own (the settings menu's draft, `runtime/ui/option_rows.cpp`
  `opt_pending_count()`).
- `cfg_value()`: the setting's value for consumers: the environment or settings-file value, else the `CfgMeta` default.
  `name` has the `NX2SARIA_` prefix, like `cfg_get()`; a name without a row is a bug (`rt_fatal()`). A row with an empty
  default has no fixed value: its consumer reads `cfg_get()` and keeps its own "unset" branch. An empty value counts as
  unset.
- `cfg_int()`, `cfg_double()`, `cfg_bool()`: `cfg_value()` parsed. A set value that does not parse (int: the whole
  string; double: strtod) gives the default's number. `cfg_bool()`: "0" is false, anything else true.

### Quality presets

The "quality presets" part of `runtime/cfg/cfg.c`; the tiers and their values are in `runtime/cfg/preset_pick.h`. A tier
is 0 low, 1 medium, 2 high (the game as shipped: sets nothing), 3 ultra. Its values are defaults: a key the player sets
wins, then the device profile, then the preset in use, then the row's default. The `preset` key holds the player's
choice (`recommended`, `low`, `medium`, `high` or `ultra`); `cfg_start()` resolves it once per start. All of these: any
thread once `cfg_start()` has returned.

- `cfg_preset_name()`: "low", "medium", "high", "ultra"; "recommended" for any other number.
- `cfg_preset_in_use()`: the tier whose values are in effect now; -1 before `cfg_start()`.
- `cfg_preset_recommended()`: the tier this PC gets (`runtime/cfg/preset_pick.h` `preset_pick()`) and, in `*why` when
  not NULL, why in a few English words.
- `cfg_preset_custom()`: 1 when a key the preset in use sets is set by the player to another value than it reads unset
  (`cfg_preset_differs()`): the menu shows the preset as custom. A key the tier leaves alone never counts.
- `cfg_preset_value()`: the value `tier` gives `key` (prefix and case ignored) on this PC; NULL when the tier leaves it
  alone.
- `cfg_preset_keys()`: every key some tier sets on some hardware (upper case, no prefix), `*n` of them; the array lives
  for the process.
- `cfg_preset_hw_changed()`: 1 when the player's tier was chosen on other hardware and this run uses the recommended
  tier instead; it stays so at every start until the player picks a tier again (`cfg_preset_chosen()`).
- `cfg_preset_chosen()`: the player picked a tier in the menu: records this PC as the one it was chosen on (the state
  file `preset-state.txt` beside the game; never `nx2saria.ini`) and clears `cfg_preset_hw_changed()`. The menu writes
  the preset key itself.
- `cfg_preset_use()`: puts `tier`'s values in effect now (the menu's switch; the restart-only ones wait for a restart,
  `cfg_pending()`), bumps `cfg_version()` and logs each setting whose value changes ("preset: <old> -> <new>: <KEY>
  <before> -> <after>"). Keys the player set keep their values: the menu resets those it means to hand to the preset.
- `cfg_preset_use_from()`: `cfg_preset_use()` for the menu's Apply, which resets the keys it hands to the preset before
  the switch: `from_values` (NULL: the values in effect now) holds the value each key of `cfg_preset_keys()` had before
  the draft's writes, in that order (an entry NULL: its value now), so the log's before -> after lines start from what
  the player saw. With `from_values` the lines are logged also when `tier` is the one in use.
- `cfg_default_tier()`: what `key` reads while the player has not set it if `tier` were in use: the device profile's
  value, else `tier`'s, else the row's default (`cfg_default()` for a tier out of range); NULL for a key without a row.
  The menu's draft shows a staged preset's values and default marks through it.
- `cfg_preset_differs()`: 1 when `tier` sets `key` and `value`, set by the player for `key`, differs from what `key`
  reads unset with `tier` in use (`cfg_default_tier()`): the key makes that tier custom, and choosing `tier` in the menu
  hands it back. 0 for NULL or "", and for a key `tier` leaves alone.
- `cfg_preset_notice_due()` and `cfg_preset_notice_done()`: the one-time notice of the recommended preset
  (`runtime/ui/menu.cpp` `preset_notice_tick()`): 1 while the player has not set the preset key, the recommended tier is
  not high, is in use, and the state file does not record a notice of that tier yet; 0 in a tool mode and a replay.
  `cfg_preset_notice_done()` records it (`noticed = <tier>` in `preset-state.txt`; never `nx2saria.ini`) and logs
  "preset: notice shown: recommended <tier> in use"; the file is written by a thread of its own, so the present thread
  that shows the notice does not wait for the disk.
- `cfg_preset_device_started()`: called by `runtime/cfg/hw_detect.c` `hw_detect_device_check()`, once the game has
  chosen its own Vulkan device: its instance made and the devices listed, as far as the probe goes, so the drivers load.
  A probing mark the state file kept from an earlier start (`runtime/cfg/cfg.c` `preset_init()`) is taken out, and the
  next start probes again. Logs "preset: the game's Vulkan device is chosen: ..." when it does; nothing without the
  mark.

## `runtime/logq.h`

Log writes on a thread of their own (`runtime/logq.c`). `runtime/main.c` owns the log's destinations and hands every
line here. The feature's description: "The log thread" in [logging.md](../logging.md#the-log-thread).

- `LogqSink` writes the bytes to the log's destinations; it may block.
- `logq_init()`: before the first `logq_write()`; lines are written by the thread that logs them.
- `logq_async()`: from now on the log thread writes them; 0 when it could not be started.
- `logq_write()`: queues the bytes, or writes them when the queue is off or has no room.
- `logq_sync()`: writes what is queued, on the calling thread, and turns the queue off.
- `logq_flush()`: writes what is queued, on the calling thread, and leaves the queue on.
- `logq_nudge()`: the log thread writes what it gathers now (a focus loss).
- `logq_peek()`: what is queued as it stands, no lock (the end nobody asked for).
- `logq_bytes()`: the queue's buffers, for the memory report (`runtime/vk/vk_memory.c` `system_log()`).

## `runtime/logq.c`

Log writes on a thread of their own.

A log line was written by the thread that logged it: to the log file and to stderr (`runtime/main.c` `log_sink()`). A
write can stall: a console that scrolls, a pipe whose reader is busy, a virus scanner, a disk that flushes; hundreds of
milliseconds at worst. The render thread logs too, and a frame it spends in such a write is a hitch with nothing else to
show for it.

With the queue on (`logq_async()`) a line is copied into a buffer under a lock that is held for the copy alone, and the
"log" thread writes the buffer out, so lines keep the order they were queued in. A line that finds no room is written by
its own thread, after everything queued before it (`put()`). `logq_sync()` writes what is queued on the calling thread
and turns the queue off for the rest of the run: a fatal error, the crash report and the end of the process call it
(`rt_log_sync()`), so no line is lost to an exit and their own lines are written as they are made.

The log thread gathers before it writes (`log_main()`): from the first line it finds it waits `LOGQ_GATHER_NS`
(250000000, a quarter of a second), or until the queue is half full (`LOGQ_GATHER_BYTES`, `LOGQ_CAP / 2`), and then
writes everything queued by then in one write. It used to write as soon as it woke, which outside a burst is once for
every line: the thread that logged woke the log thread for each, and each was a write of its own to every destination.
The price is that the log file and the console are up to `LOGQ_GATHER_NS` behind the game. The lines being gathered are
in the queue like any others, so `logq_sync()` writes them too, and a fatal error, a crash report and an exit still lose
none. A process that is ended from outside runs no code of ours and loses what was gathered at that moment; it used to
lose only the lines the log thread had not reached yet.

Every write is timed. One that took `LOGQ_SLOW_NS` (2000000, 2 ms) or longer is counted, and a line says so at most
every ten seconds (`LOGQ_SAY_NS`, 10000000000): with the queue that time was the log thread's, without it
(`NX2SARIA_LOG_ASYNC=0`) the logging thread's.

The state `Q`: the lock `q` guards `buf` and `n`; `io` guards the queue's writes and `out`; `io` is taken first. `buf`
and `out` are `LOGQ_CAP` bytes each: the queue, and what the log thread is writing. `now`: `logq_nudge()` asked for the
gathered lines at once. `slow`, `said`, `worst_ns` and `full`: slow writes, those reported, the longest; and the lines
that found no room.

### `logq_bytes()`

The queue's two buffers, made once when the log thread starts (`logq_async()`) and kept: the memory report's figure.

### `put_out()`

The write itself, timed.

### `say_slow()`

The line about slow writes, when there are new ones and the last line is ten seconds old. Called with no lock held: the
line goes through `logq_write()` like any other.

### `drain()`

Writes out what is queued, in order. Any thread.

### `put()`

1 when the bytes were queued, 0 when the caller is to write them: the queue is off, or it had no room (the log thread is
held up in a write, or the line is larger than the queue), and then what it held has been written first (`drain()`). So
a thread's lines reach the log in the order it logged them.

The queue's lock is waited for. A thread that wrote its line itself because the lock was taken would put that line
before the lines it queued earlier, and on a busy machine the lock is taken for a whole time slice whenever its holder
loses the processor inside the copy. Waiting cannot hang: the lock is held for the copy alone, the runtime stops a
thread only for moments in which it takes no lock and logs nothing (`runtime/hle/kernel.c` `ap_try()` and `dg_sample()`,
`runtime/diag/prof_win.c`), and a fatal error or a crash turns the queue off before it logs (`logq_sync()`, which only
tries the locks).

The log thread sleeps until the first line of a batch, and then until the batch fills half the queue (`log_main()`):
those are the two moments `put()` wakes it.

### `log_main()`

The log thread: it sleeps until a line is queued, gathers, and writes the batch (`drain()`). While it gathers it holds
no lock, so neither the threads that log nor `logq_sync()` wait for it. The gathering ends when its time is up, when the
queue is half full (`put()` signals that), and when the queue is empty again because another thread wrote it: a line
that found no room (`put()`), or `logq_sync()`, after which nothing is queued any more and this thread sleeps for good.

### `logq_flush()`

What is queued, written now on the calling thread, with the queue left on: a point the log is to have reached on the
disk before something slow comes (the orderly end before its pipeline save, `runtime/gfx/gfx_present.c`
`rt_exit_steps()`). Each lock is tried for at most 100 ms: the log thread may be stuck in a write and hold `io`. Without
both locks nothing is written here, since other threads still queue and write, and the log thread writes the lines when
it can.

### `logq_nudge()`

The log thread writes what it gathers now instead of when the gathering time is up: a focus loss, after which the
process may be ended from outside (`runtime/main.c` `rt_log_focus_lost()`); such an end loses what was being gathered.

### `logq_peek()`

What is queued, as it stands, with no lock: for the note of an end nobody asked for, written with every other thread
stopped (`runtime/platform/platform.h` `plat_exit_note_setup()`). Nothing once the queue is off.

### `logq_sync()`

Each lock is tried for half a second: the thread that holds it may be the one that crashed, or be stuck in the write
this call is there to get past. Without the lock the queue is written as it stands.

## `runtime/log_stall.h`

What the log keeps so that a stall, a freeze or a lost device can be traced to the minute afterwards
(`runtime/log_stall.c`; "Tracing a stall in the log" in [performance.md](../performance.md#tracing-a-stall-in-the-log)):
the local time of the lines that mark an event, counts for the events whose own lines are limited, and the machine's
memory and I/O pressure once a minute.

- `log_stamp()`: the local time as "hh:mm:ss", the clock the system's journal and the kernel log use, of a moment on
  `plat_now_ns()`'s clock (0: now). "--:--:--" when the zone cannot say.
- `LsEvent`: the events whose own lines the log limits or leaves out, counted for the frame-time summary's "events"
  line.
- `log_event()`: counts one (any thread, no lock). 1 while the event's own line is to be written: its first few times (5
  acquire failures, 3 watchdog firings, the first MapPhysicalMemory and UnmapPhysicalMemory failure), 0 after them.
- `log_events_text()`: the events as the text of one line: those counted since the previous call without `whole`, each
  with its total and the limit of its own lines ("presenter acquire failed 2 (7 in all, only the first 5 logged)");
  `whole`: every event of the run, the previous call's moment left alone. 0 (and an empty string) when there is none.
- `log_pressure_text()`: the machine's memory and I/O pressure as the text of one line, against the sample of the
  previous call (the baseline, or the line before it), so the stall time of the minute is a difference of the system's
  totals. 0 and an empty string when the platform gives nothing or the sampling is busy on another thread (never waited
  for). `log_pressure_baseline()` takes the sample the first line is made against: the start of the first minute.
- The stand-ins at the end of the header: a program that compiles a file of the runtime without `runtime/log_stall.c`
  (the unit checks in `tests/` do, with stand-ins for the rest) links these: no time ("--:--:--"), every event has its
  line, no counts, no pressure. `runtime/log_stall.c` replaces them as it defines `LOG_STALL_IMPL` first; the game's
  link takes the whole runtime library, so its definitions win.

## `runtime/log_stall.c`

The log had one wall-clock line, the window's focus change with its date (`runtime/gfx/gfx_present.c`). A report of a
stall, or of a GPU that stopped, was matched against the system's journal and kernel log by an estimate (about 2 s off
at best), and three things hid how often an event happened: the lines of the presenter's failed acquires and of its
watchdog stop after 5 and 3, and the physical memory map's refusals (a session had 132 of them) wrote one line each.
Nothing said whether the machine was short of memory or waiting for its disk while the frames stopped. This file gives
each of those one place:

- `log_stamp()`: the local time, "hh:mm:ss", of a line that marks an event;
- `log_event()` and `log_events_text()`: a count for each limited event, and the one line the frame-time summary writes
  with the counts of its minute and, at the end of the run, of the whole run;
- `log_pressure_text()`: the memory and I/O stall times of the minute (Linux pressure stall information), the memory
  available and the swap free, or on Windows the memory available and the commit.

The file defines `LOG_STALL_IMPL` before it includes its header: it defines what the header's stand-ins stand for.
`ev_total[]` holds every count of the run and `ev_seen[]` the totals the previous window line counted from. `pr_prev`
(under `pr_lock`) is the sample the next line is made against.

### `put()`

Appends to `out` at `*n`, never past `cap`.

### `psi_text()`

One resource's text: the average of the last 10 s and, with a sample to compare (`prev`), what the totals gained since
it.

### `pressure_format()`

The text from a sample, the sample before it (NULL: none) and the seconds between them; `hms` is the time of the sample.
Linux: PSI memory and io, MemAvailable and SwapFree. Windows (the branch on `avail_mb`): the memory available and the
commit.

## `runtime/log_privacy.h`

Personal folders written short in what players share (`runtime/log_privacy.c`): the game log, stderr, the bug capture's
files and the crash report. `C:\Users\<name>\AppData\Local\NX2Saria\game` becomes `%LOCALAPPDATA%\NX2Saria\game`,
`/home/<name>/Games` becomes `~/Games`, the account's name alone inside a path becomes `%USERNAME%` or `$USER`, and the
computer's name `%COMPUTERNAME%`. On by default; `log_short_paths = 0` writes them in full. Paths handed to other
programs (the tracker API's `save_dir`, the files the game opens) are never changed: only the text written for people to
read. The feature's rules for the player: [log-privacy.md](../log-privacy.md).

- `log_privacy_init()`: called by `runtime/main.c` `main_run()`, first: the folders from the environment; no allocation.
- `log_privacy_start()`: after the settings are read (`mem_init()`): the switch, the folders' resolved forms, a line.
- `log_privacy_find()`: where the first personal folder starts in `p` (`n` bytes), or `n` when it holds none or the
  switch is off. Counts the texts that hold one (`log_privacy_count()`).
- `log_privacy_rewrite()`: `p` (`n` bytes) from `*at` on into `out` (`cap` >= 64 bytes) with each folder short; the
  bytes written. `*at` moves past the bytes read: a caller loops until `*at == n` (`runtime/log_privacy_core.h`
  `priv_rewrite()`).
- `log_privacy_str()`: a whole string short into `out` (`cap` bytes), for one value in a file the game writes for people
  (the bug capture); returns `out`, or `s` itself when it has nothing to shorten or the switch is off. Cut at `cap - 1`
  bytes.
- `log_privacy_str_always()`: the same with the switch off too, for text that leaves the PC
  (`runtime/session_summary.c`: the session summary the launcher may send): `log_short_paths = 0` is for one's own logs,
  never for what goes to a server.
- `log_privacy_dup()`: a malloc'd copy of `p` (`n` bytes) with each folder short, `*out_n` its length; NULL when it has
  nothing to shorten, the switch is off or memory runs out (the caller keeps `p`). Not for the crash report: it
  allocates.
- `log_privacy_count()`: the lines and values written short so far.

## `runtime/log_privacy.c`

Personal folders written short in what players share: the game log (`runtime/main.c` `rt_log_raw()`, so the log file,
stderr, the queue the exit note reads and the crash report alike), and the values the bug capture writes
(`runtime/diag/bugcap.c`). Players attach `logs/nx2saria.log` and the F8 capture's `log-tail.txt` to public issues, and
the log named files under the account's folder: `C:\Users\<name>\AppData\Local\Programs\NX2Saria\game\SDL3.dll`,
`/home/<name>/Games/...`

The folders come from the environment once (`log_privacy_init()`, before the first line), and the matching is
`runtime/log_privacy_core.h`: a table read in one pass, with no allocation and no lock, so the crash report goes through
it too.

- Windows: `LOCALAPPDATA`, `APPDATA`, `USERPROFILE`, `HOMEDRIVE` + `HOMEPATH`, `HOMESHARE` (a profile on a network
  share), `OneDrive`, `TEMP` and `TMP` (often written in the 8.3 form, `C:\Users\ANASOU~1\...`), the 8.3 forms of
  `USERPROFILE` and `LOCALAPPDATA`, and under Wine the Unix home (`HOME`, `Z:\home\<name>`, `WINEHOMEDIR`); the name is
  `USERNAME`, the computer `COMPUTERNAME` and `USERDOMAIN` (the computer's own name on a PC outside a domain), and under
  Wine also the Linux host name (`HOSTNAME`).
- Linux: `HOME` and the folder it resolves to (realpath: `/var/home/<name>` on systems where `/home` is a link); the
  name is `USER` or `LOGNAME`, the computer gethostname (and its first label) and `HOSTNAME`.

Computer names often hold the owner's name ("maria-pc"); they are written `%COMPUTERNAME%` (`%USERDOMAIN%` for a
domain's name) as a word of their own anywhere in a line. The markers are the ones a player can type back
(`%LOCALAPPDATA%`, `~`). A line with no personal folder costs one table read per byte (`log_privacy_find()`) and is
passed on as it is.

`log_short_paths = 0` (`NX2SARIA_LOG_SHORT_PATHS=0`) writes everything in full, for debugging on one's own machine. The
lines before the settings are read are written short either way.

`rt_log()` is declared in this file alone, not through `runtime/host.h`: that header brings the CPU emulation's headers,
which `tests/log_privacy_env_check.c` leaves out.

### `add_host()`

A host name, and its first label when it has a domain (maria-pc.lan: maria-pc too).

### `add_short()`

The 8.3 form of an existing folder (`C:\Users\ANASOU~1`), which `TEMP` and paths from older programs use.

### `log_privacy_init()`

- Wine hands the Unix environment on: the Unix home, also as the Windows path `Z:\home\<name>`.
- `USERDOMAIN` is the computer's own name on a PC outside a domain, else the domain's.
- `HOSTNAME` on Windows: Wine hands the Linux environment on.
- On Linux the host name comes from gethostname (uname: a system call, no allocation).

### `log_privacy_find()`

The count goes up for every text that holds a folder: every caller rewrites what it finds.

### `log_privacy_dup()`

The buffer doubles when the rest did not fit.

## `runtime/log_privacy_core.h`

Personal folders written short (`runtime/log_privacy.c`): the part that makes no system call, so that
`tests/log_privacy_check.c` builds it on any system with Windows and Linux tables alike.

A table holds the folders to write short (prefixes, such as `C:\Users\<name>\AppData\Local`) and the account's name.
`priv_find()` says whether a text holds any of them; `priv_rewrite()` copies the text with each one replaced by its
marker (`%LOCALAPPDATA%`, `%USERPROFILE%`, `~`, `%USERNAME%`). The rest of each path is kept: it tells where a file was.

Matching, in one pass over the bytes (`first[]` marks the bytes a pattern can start with, so most bytes cost one table
read):

- A prefix matches where the byte before it is not part of a name (a letter, digit, '_', '.', '-' or a byte of a UTF-8
  sequence) and the byte after it is not either: `/home/ana` does not match in `/home/anabel` nor in `/var/home/ana`,
  and `\\?\C:\Users\ana\x` matches. The longest prefix that matches at a place wins (`%LOCALAPPDATA%` before
  `%USERPROFILE%`).
- The account's name, three bytes or longer, matches as a word of its own (no letter, digit, '_' or UTF-8 byte on either
  side) inside a path: the run of non-blank bytes around it holds a '/' or '\'. "Steam Deck" stays as it is for the
  account `deck`; `/tmp/run-deck/x` and `D:\deck\saves` become `/tmp/run-%USERNAME%/x` and `D:\%USERNAME%\saves`.
- A table with `fold` = 1 (Windows) compares letters without case (ASCII), takes '/' and '\' as the same byte, and a run
  of separators as one (`C:\\Users\\ana`, a path as an escaped string writes it).
- A '.' or '-' right after a folder ends it when no name byte follows (the end of a sentence: "saved in /home/ana.").
- The computer's name, three bytes or longer, matches as a word of its own anywhere (no path needed: a UNC path
  `\\<host>\share`, a network line), without case on every system (host names have none). `maria-pc.local` becomes
  `%COMPUTERNAME%.local`; names a fresh install gives every machine (`localhost`, `steamdeck`, ...;
  `priv_common_host()`) are not looked for.
- Names too common to be the account's alone (`user`, `admin`, `deck`, ...; `priv_common()`) are not looked for alone: a
  path under their home still matches as a folder, and `/run/user/1000` stays as it is.

No allocation and no lock: the crash report writes its lines through here.

- `PRIV_MAX`, 24: the patterns in a table.
- `PRIV_PAT_MAX`, 520: the bytes of one pattern (a Windows path of MAX_PATH UTF-16 units is up to 3x that; longer
  folders are skipped, see `priv_add()`).
- `PRIV_MARK_MAX`, 16: the bytes of one marker.
- `PRIV_NAME_MIN`, 3: an account name shorter than this is not looked for alone.
- `PrivPat.pat` is folded when the table folds (lower case, '/' for '\'). `PrivTable.fold` is 1 for Windows paths (no
  case, either separator); `first[256]` is 1 where a pattern can start with that byte (both cases when folding).

### `priv_name_byte()`, `priv_path_byte()`

A byte that continues a word (a digit, a letter, '_', or 0x80 and above); a byte that continues a folder's name (those,
and '.' and '-').

### `priv_common()`

Account names that are words of their own in paths that are not the account's (`/run/user`, "User Data").

### `priv_common_host()`

Computer names a system gives every machine of its kind: not the person's.

### `priv_add()`

Adds a pattern. A prefix loses its trailing separators; one that is empty, a root ("/", "C:\", "\\") or too long is not
added, nor a name shorter than `PRIV_NAME_MIN`, nor a pattern the table holds already. Kept longest first, so the
longest match at a place is found first. 1 when added. A root, or a drive's root ("C:"), would write every path short.

### `priv_folder_end()`

The byte after a folder ends it: none, a byte that is not part of a name, or a '.' or '-' with none after it.

### `priv_match()`

The pattern that matches at `s[i]` (`s` holds `n` bytes), or NULL; `*len` gets the bytes of `s` it covers (more than the
pattern's when a folded table took a run of separators for one, as in `C:\\Users`). For a name, `path` says whether the
non-blank run around the name holds a separator.

### `priv_find()`

Where the first pattern starts in `s`, or `n` when `s` holds none.

### `priv_rewrite()`

Copies `s` (`n` bytes) from `*at` on into `out` (`cap` bytes) with each pattern replaced by its marker, and returns the
bytes written; `*at` moves past the bytes read. It stops before a piece that would not fit, so a caller with more to
copy writes out and calls again; with `cap` >= `PRIV_MARK_MAX` every call reads at least one byte. The whole text is
passed each time so that the byte before `*at` still counts for the match there. `*hits`, when given, is increased by
the patterns replaced.

## `runtime/session_summary.h`

The session summary: `logs/session-summary.json`, written once at the game's end (`runtime/session_summary.c`) for the
launcher, which sends it while session reports are on ([telemetry.md](../telemetry.md)). The game sends nothing itself.
What the comments of these three files say about what leaves the player's machine is given here in full; the player's
side of the same rules is in [telemetry.md](../telemetry.md#what-a-session-report-holds).

Two parts. `runtime/session_summary_fmt.c` turns a `SessionSummary` into the JSON text and needs nothing of the runtime
(the unit check `tests/session_summary_check.py` builds it alone). `runtime/session_summary.c` gathers the values and
writes the file.

### `SessionSummary`

The values, as the formatter takes them. A string NULL, an integer below 0 and a number below 0 are written as null.
`version`, `game_version` and `end` are always written (an empty string when NULL).

- `os_family` is "windows" or "linux". `wine` is 1, 0, or -1 for null.
- `gpu_vendor` and `gpu_device` above 0xffff are written as null.
- Tokens: the display's and the settings' values (`output`, `render_scale`, `game_aspect`, `picture`, `preset`,
  `upscaler`, `taa`, `dlss`, `framegen`, `vk_present`). A value with a character outside `[A-Za-z0-9_.:x+-]` or longer
  than 32 is written as "other"; `ss_setting()` maps a value outside the setting's own list to "other" before. `picture`
  is the size of the picture the game draws, "2560x1440" or a widescreen one (`runtime/patches/widescreen.c`), as "WxH".
- `fullscreen` is 1, 0, or -1 for null.
- `end` is "normal", "fatal" or "device_lost".
- `has_frametime` 0 writes "frametime" with every number null.
- `patches`: the port's patch sets that are on (`runtime/patches/patches.c`), by name: `n_patches` of them, below 0 for
  null. A name that is not 1 to 32 of `[a-z0-9_]` is left out, and no more than 96 are written.
- `mods_count` and `mods_code`: the mods in use, and those of them that change the game's code
  (`runtime/patches/mods.c`). Never their names.
- `fatal`: the FATAL line without "FATAL: ", personal folders already short.

### `ss_format()`

The JSON text into `out` (`cap` bytes, NUL-terminated); its length, or -1 when it does not fit. It escapes every string,
drops control characters and invalid UTF-8, cuts `os.name`, `cpu.name`, `gpu.name` and `gpu.driver` at 64 characters and
`fatal` at 300, and writes the three frame-time numbers with at most one decimal. No allocation.

### `ss_setting()`

A value as the summary writes a setting: the list's own spelling when `value` (any case) is one of `allowed`'s items
("a|b|c"; an item "lo..hi" takes a whole number in that range), "other" when it is not; `value` itself when `allowed` is
NULL (free text). NULL for a NULL or empty value. The result is `value`, an item copied into `out` (`cap` bytes), or a
static string.

### `ss_os_name()`

The OS line of `runtime/platform/platform.h` `plat_os_version()` as the summary names the OS: "Windows 10.0.26100.1234"
becomes "Windows 11 (build 26100)" (build 22000 and later are Windows 11), "Ubuntu 24.04.1 LTS (Linux 6.8.0)" loses the
kernel part. Anything else is copied. `out` gets `cap` bytes.

### `ss_frametime()`

The session's frame times from a histogram of `bins` bins of `bin_ns` each (the last also takes every slower frame), as
`runtime/diag/perf.c` `ft_report()` computes them: mean fps over the summed intervals, the 1% low as the upper edge of
the bin where the slowest 1% of frames begins, the slowest frame in ms. All 0 when `frames` or `sum_ns` is 0.

### What the back end hands over

Strong definitions are in `runtime/vk/vk_caps.c`, `runtime/gfx/gfx_shader.c` and `runtime/vk/vk_pipeline.c`;
`runtime/session_summary.c` has weak ones returning 0, for builds without the Vulkan back end.

- `SsGpu.vram_mb` is the largest device-local heap. `vk_caps_gpu()`: 1 once `vk_caps_init()` ran; it reads values stored
  then, with no driver call. Any thread.
- `gfx_shader_prewarm_stats()`: the boot's shader prewarm (`runtime/gfx/gfx_shader.c` `gfx_shader_prewarm()`): its time
  and the shaders it translated. 1 once it ended.
- `vk_pipeline_boot_stats()`: the boot's pipeline prewarm (`runtime/vk/vk_pipeline.c`): the pipelines it built and the
  time of its pipeline stage. 1 once it ended.
- `wide_picture_known()`: the picture `runtime/patches/widescreen.c` `wide_init()` set up: 1 and its size once
  `wide_init()` has decided, 0 before (no computing: the fatal end may call it on any thread).
- `rt_patches_on()`: the port's own patch sets that are on (`runtime/patches/patches.c`): up to `cap` of their names
  into `names` (pointers valid for the process), not the sets a mod's ExeFS patches were built into (`mod_<hash>`).
  Their number, or -1 before `rt_patches_init()`.
- `mods_in_use()`: the mods in use this run (`runtime/patches/mods.c`: on at the scan and without a problem) and how
  many of them change the game's code with ExeFS patches. 1 once the scan ended; never scans.

### The game's side

- `session_summary_start()`: called by `runtime/main.c` `main_run()`, a game run only, after the log is open: notes the
  start, the OS, CPU and RAM, and removes a summary and a header an earlier run left, so that the launcher never takes
  them for this run's. Before this the other calls do nothing.
- `session_summary_header()`: `logs/session-start.json`, the header the launcher builds a hard crash's summary from
  (`launcher/telemetry.py` `synthesize_crash()`). `at` names the moment for the log line. Called at four moments only,
  never per frame: the end of `session_summary_start()` (the OS, CPU, RAM and settings known, before any Vulkan
  device), `runtime/vk/vk_device.c` `create_device()` after `vk_caps_init()` (the card), `runtime/vk/vk_pipeline.c`
  `vk_pipeline_prewarm()` (the boot's prewarm numbers), and `runtime/ui/option_rows.cpp` `opt_draft_apply()` after the
  settings file is saved (once per Apply). Nothing after the summary was written.
- `session_summary_exit()`: the orderly end (`runtime/gfx/gfx_present.c` `rt_exit_steps()`), after the frame-time
  summary: end "normal".
- `session_summary_fatal()`: a fatal error's end (`runtime/dispatch.c` `fatal_end()`), after its frame-time summary, on
  the failing thread: end "fatal", or "device_lost" for code 72 (`RT_LOST_EXIT` of `runtime/dispatch.h`). `line` is the
  FATAL line's text. Static buffers, no allocation.

## `runtime/session_summary.c`

The session summary's gatherer (`runtime/session_summary.h`): what the game knows about the run at its end, written once
to `logs/session-summary.json` beside `logs/nx2saria.log`. The launcher reads it after the process ends; the game sends
nothing. A hard crash writes none (`runtime/main.c` `crash_handler()`): the launcher builds that one from the log's
FATAL line and from the header, `logs/session-start.json`, which this file writes during the run with the same
fields as far as they are known then (`session_summary_header()`).

What cannot change during the run (the OS, CPU and RAM, the version) is read at the start, on the main thread. The rest
is read at the end: the GPU from values `runtime/vk/vk_caps.c` stored when the device was made (no driver call, which a
lost device could hang), the settings through `cfg_value()` (the live values: a change made in the menu counts), the
frame times from the session histogram of `runtime/diag/perf.c` after its `ft_stop()`, and the boot's prewarm numbers
from their stores. All into static buffers: the fatal end runs this on a failing thread that may hold any lock, where
allocation is not safe.

The weak `vk_caps_gpu()`, `gfx_shader_prewarm_stats()` and `vk_pipeline_boot_stats()`: builds without the Vulkan back
end (the null renderer's tools) have no device and no boot prewarm. The weak `wide_picture_known()`: builds without the
widescreen sets' code (the tools) have no picture of their own.

The FATAL line goes through `log_privacy_str_always()`: its personal folders are written short even with
`log_short_paths = 0`, since that switch is for one's own logs, never for what goes to a server
([log-privacy.md](../log-privacy.md#the-switch)).

### `session_summary_start()`

- The file goes beside the log file; a run whose log goes to a redirected stderr (`tools/run_game.py`, a script) has
  none, and its summary goes to `logs/` beside the executable, where the launcher looks (`runtime/main.c` `log_open()`
  makes the same folder). The path is cut at the end of ".../logs", at its separator before the file's name.
- An earlier run's summary goes now: a crash writes none, and the launcher must not read the old one as this run's.
- A release's version carries its commit after a "+" (`rt_port_version` of `runtime/host.h`); the summary keeps the
  release.
- `cfg_preset_custom()`'s first call makes its key list under a lock: made here, on the main thread, the fatal end's
  call takes none (`cfg_start()` chose the tier before this, `runtime/memory.c` `mem_init()`).
- The header's path is the summary's with the file name changed; both files go at the start. The first header is
  written last, once `armed` is set.

### `gather()`

The values of a summary, the same for the summary and the header, into a `Gathered` of buffers the caller owns (the
summary's and the header's are two static ones, so a header written at the moment of a fatal end does not write over
the summary's). `whole` 0 leaves `session.duration_s`, `session.frames` and the frame times null: a header must not
carry a part of the session as if it were the whole. Returns whether the card was known (`vk_caps_gpu()`).

### `session_summary_header()`

- The same `SessionSummary` as the summary's, from the same getters, through the same `priv()` and `ss_format()`, so a
  header holds no field and no value a summary could not; `session.end` is "running", which the launcher replaces.
- `plat_file_write_atomic()`: a temporary file in the same folder, renamed over the old header, so a crash during the
  write leaves the previous header whole or none.
- `header_lock` keeps two headers (the device thread's and the menu's, say) from sharing the buffers. The summary's
  own write never takes it: the fatal end runs on a thread that may be the one holding it.
- Each write logs `telemetry: session header written to logs/session-start.json at <moment> (<bytes> bytes; gpu
  known|not yet known; startup known|not yet known)`, or `telemetry: session header not written at <moment>: <why>`.
  The launcher uses the file only when the run's log holds the first line, so the line is the proof that the file is
  this run's: the log starts new with every start of the game (`runtime/main.c` `log_open()`), and the new run removes
  the old header before it writes its own. `HEADER_LINES` (8) of these lines per run, then
  `telemetry: session header: 8 writes logged; later ones are not`; Apply is the only moment that can repeat.

### `setting()`

A setting's value as the summary writes it (`runtime/session_summary.h` `ss_setting()`); NULL for a key without a row in
`runtime/cfg/cfg.c`, which `cfg_value()` would end the process on.

### `write_summary()`

- The values come from `gather()` with `whole` 1.
- The quality preset in use (the "quality presets" part of `runtime/cfg/cfg.c`): the tier's name, the one "recommended"
  stands for included, or "custom" when the player changed a setting the tier sets, as the menu shows it; null before
  `cfg_start()` chose one.
- `fps_limit` is a number of frames per second; "display" and "unlimited" have none, and "other" is not one.

## `runtime/session_summary_fmt.c`

The session summary's JSON text (`runtime/session_summary.h` `ss_format()`) and the small value rules it shares with the
gatherer (`ss_setting()`, `ss_os_name()`, `ss_frametime()`). Plain C with no runtime calls:
`tests/session_summary_check.c` builds this file alone. No allocation and no locale-dependent formatting, since
`runtime/session_summary.c` runs it on a failing thread at a fatal error. `Buf.full` says something did not fit.

### `put_int()`

Whole numbers through snprintf's `%lld`, which no locale changes; a value below 0 is null, above `max` is `max`.

### `put_dec1()`

One decimal, rounded, made from whole numbers: printf's `%f` takes the decimal separator of the C locale, and a library
that sets another one would write "59,9". NaN and values below 0 are null, values above `max` are `max`.

### `utf8_len()`

The length of the UTF-8 sequence at `s` (`n` bytes left) when it is a valid one and not a C1 control (U+0080-U+009F); 0
otherwise. Overlong forms, surrogates and code points above U+10FFFF are invalid. The second-byte tests: after 0xC2
below 0xA0 is a C1 control; after 0xE0 below 0xA0 is overlong; after 0xED 0xA0 and above is a surrogate; after 0xF0
below 0x90 is overlong; after 0xF4 0x90 and above is past U+10FFFF.

### `put_text()`

A free-text string as a JSON string of at most `max` characters (code points): quotes and backslashes escaped, tabs and
line ends as one space, other control characters and invalid UTF-8 dropped, spaces at both ends left out (spaces are
held back until a character follows them).

### `put_token()`

A display or settings value: 1 to 32 characters of `[A-Za-z0-9_.:x+-]`, else "other"; NULL is null.

### `put_required()`

The fixed values (schema, versions, end): the token rules, with "" for NULL instead of null.

### `ss_format()`

With no frame counted, "frametime" is an object of nulls, as every other group; the server takes no null group.

### `whole()`

A whole number of at most 9 digits, else -1.

### `ss_setting()`

For an item "lo..hi": the value as a whole number, written without leading zeros or sign.

### `ss_os_name()`

`runtime/platform/linux.c` `plat_os_version()` adds the kernel's release after the name, as " (Linux ...)", which is cut
here.

### `ss_frametime()`

The cut is the slowest 1% of frames, `(frames + 99) / 100`; the 1% low is the upper edge of the bin where it begins.

## `runtime/discord_app.h`

Discord Rich Presence can only use an application ID registered by its owner. No NX2Saria Discord application has been
verified, so the defaults stay empty. A player may provide their own ID as `NX2SARIA_DISCORD_APP_ID` (or the legacy
`NX2NAVI_DISCORD_APP_ID`) or `discord_app_id` in `nx2saria.ini` / `nx2navi.ini`. Discord displays the name and art
registered to that ID; the game does not claim or supply an NX2Saria application.

## `runtime/discord_rpc.c`

Discord Rich Presence: an activity with elapsed play time in the player's Discord status, through the local IPC of a
running Discord client (the `plat_ipc_*` calls of `runtime/platform/platform.h`). Nothing leaves the machine but the
activity itself, and nothing is sent while `NX2SARIA_DISCORD_RPC` is off (it is on by default, and does nothing without
a Discord client running on the same PC). `NX2SARIA_DISCORD_APP_ID` names the player's application whose registered name
and artwork Discord shows; `NX2SARIA_DISCORD_IMAGE` is its uploaded art key. Both may also be set in either
`nx2saria.ini` or a legacy `nx2navi.ini`. No app ID is built in. With no ID the thread stays idle and logs that once.

One host thread owns the connection and never holds a virtual core (`runtime/hle/kernel.c`: host work runs without one).
It re-reads the settings when `cfg_version()` changes (a menu change reaches it within half a second), connects to the
first of Discord's `discord-ipc-0` to `discord-ipc-9` pipes that answers, and writes the activity again every 60 s: that
write is the keep-alive, and one that fails closes the connection and tries again 15 s later. Discord clears the status
by itself when the process ends and the pipe closes, so there is no shutdown path. Every answer frame is read and
discarded: unread, they would fill the pipe's buffer and block the client's writes.

The protocol is the documented IPC one: a frame is <opcode u32 little endian><length u32><length bytes of JSON>, with
Handshake 0, Frame 1, Close 2, Ping 3 and Pong 4. The handshake (opcode 0) carries `{"v":1,"client_id":...}` and is
answered by a Ready frame (opcode 1); every command, SET_ACTIVITY here, is a frame (opcode 1) answered by one, and a
Ping the client sends is echoed as a Pong. A client that speaks another numbering (an early alternative client did)
answers the handshake with a Close. `--probe-discord` runs the same exchange once, without the game and without the
setting being on; it needs no game files.

- `APP_ID_CAP`, 24: a Discord snowflake is at most 20 digits.
- `ANSWER_CAP`, 8192: the Ready frame carries the client's config; a few KiB.

### `valid_app_id()`, `valid_image_key()`

A settings value that fits the JSON without escaping: the app id is digits, an asset key letters, digits, '_' and '-'
(what Discord's developer portal accepts for an asset key).

### `read_setting()`

The setting's value, or its default when the value does not fit the protocol (logged: the player sees why their own app
does not show), or "" when the default does not fit either. Called on the presence thread and by the probe, after
`cfg_version()` changed or once.

### `read_one_frame()`

One frame off the wire, its 8-byte header in as many reads as the peer made, then its payload; a payload longer than
`cap-1` fills `buf` with its first `cap-1` bytes and the rest is still consumed. `*op` gets the opcode; `buf` is
NUL-terminated. 1 on a whole frame, 0 when the deadline passed, -1 when the connection broke.

### `read_frame()`

The next frame that is not a Ping: a Ping the client sent to see we are alive is echoed as a Pong with its own payload,
a Close (the client told us it is done) counts as a broken connection so the caller connects again later. The same
return values as `read_one_frame()`; the deadline covers the whole exchange.

### `connect_client()`

The first of Discord's pipes that completes a handshake. NULL when no client is running or none answered in time; the
caller tries again later. The answer wanted is the Ready event; a pipe that answered but not with a Ready frame in 2 s
(2000 ms) is closed: the next may belong to a client.

### `send_activity()`

The activity as one SET_ACTIVITY frame: the game update and the port version as the details line ("Game 1.1.1 · Recomp
0.3.7": the port version without its +commit suffix, the art's hover text keeps the full one; the dot goes as a JSON
escape), the process's start as the timestamp Discord counts the elapsed time from, and the application's art asset. 1
when the client answered (a slow answer that ran out of the 2 s read is fine: the next drain takes it), 0 when the
connection broke.

### `discord_main()`

The presence thread. When the settings changed (the menu writes them) it picks the new ones up at once. Any setting
bumps the version (a slider step, a cheat): only a new app id needs a new handshake, and a new image only a resend;
reconnecting on every change made the status blink and could hit Discord's rate limit while a slider was held. On, but
with no application to show it under, it waits for a settings change.

## `runtime/mem_budget.h`

How much room the card and the system have, as one level per side (`runtime/mem_budget.c`): roomy, normal, tight or
critical. Owners of memory that can be given back read the level and give back earlier when room is short; what they
keep while there is room is theirs to decide. The header-only part is the arithmetic, which `tests/mem_budget_check.c`
runs.

A side's room is its free share in per mille: for the video memory the driver's budget less this process's use of the
device-local heaps (`runtime/vk/vk_memory.c` `vk_mem_vram()`); for the system the lower of the commit's headroom (the
commit limit less the system's commit in use: an allocation fails at the limit) and the physical memory free
(`plat_commit_info()`, `plat_mem_info()`). The levels begin at fixed shares going down, and a side climbs back to a
better level only once its share is `MEM_HYST_PM` above the line it fell through, so a share that hovers at a line does
not flip the level at every sample. Below `MEM_FLOOR_MB` free a side is critical whatever its share.

- `mem_level_line[]`, {400, 200, 80}: the free share (per mille) above which a level holds, from the best down: roomy
  above 400 (40% free), normal above 200, tight above 80; critical at 80 or less. The 20% line is where the VRAM
  pressure rule of `runtime/vk/vk_texture.c` (`tex_vram_pressure()`, 80% of the budget in use) starts to act on released
  textures.
- `MEM_HYST_PM`, 50: a level is left for a better one 5 points above the line it fell through.
- `MEM_FLOOR_MB`, 256: less free than this is critical at any share.
- `MemRoom`: `free_pm` is the free share of the limit, per mille; `free_mb` the free MiB.

The declarations of `runtime/mem_budget.c`: the levels, sampled every `MEM_BUDGET_PERIOD_MS` (2000) on a thread of their
own, off the drawing threads. Each change of a side's level is logged with its figures. `MEM_BUDGET_TEST_VRAM_MB` and
`MEM_BUDGET_TEST_RAM_MB` (0: off) pretend a side is that small, for testing what gives memory back on a large machine:
the video memory budget becomes at most that many MiB against this process's use, and the system's room becomes that
many MiB less this process's commit. They change the levels only, never the texture cache's own rules.

- `mem_budget_vram()`: called by `runtime/vk/vk_device.c`: 1 once the device is up (the first call starts the thread), 0
  before it goes (waits for a sample under way).
- `mem_budget_level()`: `MEM_ROOMY` to `MEM_CRITICAL` of `MEM_VRAM` or `MEM_RAM`; `MEM_NORMAL` before its first sample.
- `mem_budget_text()`: "video memory normal (31.2% free), system memory roomy (55.0% free)".

### `mem_room()`

The room of a side with `used` of `limit` in use (any one unit, here MiB); none when `used` reaches the limit.

### `mem_room_min()`

The tighter of two measures of one side: the lower share, and the lower free amount.

### `mem_level_next()`

The level a side moves to from `level` at room `r`: down to a worse level at once, up only past the hysteresis. It
climbs one level at a time while the share clears the line above with room to spare.

## `runtime/wb_lazy_choice.h`

Whether the lazy write-back runs ("Lazy write-back" in the notes of `runtime/memory.c`, `runtime/vk/vk_memory.c`
`vk_mem_lazy_decide()`), from the player's setting, the platform and the GPU driver, with the reason the log gives. It
is a header of its own so that `tests/wb_lazy_check.c` checks it without Vulkan.

The lazy write-back makes pages of guest memory inaccessible while a host-pointer import of them is bound to a buffer
the GPU writes, and copies their bytes down from inside the access-fault handler of whichever thread touched them. A
driver is free to handle both differently, so the default is on only where it has been played:

- Windows, AMD's own driver: off. In the days after 0.7.1 turned it on for every card on Windows, two reports from AMD
  cards had the game end in play. An RX 6600 lost its device 3.5 minutes in with it on (146 fetches in the 1,200 frames
  before; the GPU's last mark was inside a draw pass, not a fetch). A player whose game ended in fights, on a card they
  did not name, then played for hours with a settings file that turns it off. Neither proves it the cause: the player
  changed several settings at once, cards with 8 GB lost their device the same way on NVIDIA before it existed, and no
  failure path was found in the code. But fights are where it is busiest (shaders write, the CPU reads back, every read
  faults and fetches), and nobody has played one on an AMD card under our eyes: builds with the default on were measured
  on an RX 7900 XT (driver 26.10.1) standing in Hyrule Field and walking through a village (`runtime/vk/vk_exec_xfer.c`
  `oq_take()`, `runtime/vk/vk_exec_draw.c`), which says nothing about a fight, and none of us has a card of the driver
  branch for RDNA 1 and 2. Since the second report's card is unknown, the default is off on both of AMD's branches until
  a card of each has played fights with it on and a log to show. The cost is the frame rate in open areas, back to what
  it was before 0.7.1 (about 65 fps against 90 standing in Hyrule Field on an RTX 5090; not measured on AMD).
- Windows, any other driver: on, as 0.7.1 shipped it. Measured and played on NVIDIA's; no report from Intel's.
- Elsewhere: off, as before. The fetch inside the fault handler has only run on Windows.

A value the player sets wins everywhere: `wb_lazy = 1` turns it on for an AMD card, `wb_lazy = 0` off for any.

- `WBL_DRIVER_AMD`, 1: the `VkDriverId` of AMD's own driver, `VK_DRIVER_ID_AMD_PROPRIETARY` (`runtime/vk/vk_memory.c`
  asserts the value).
- `WblChoice.why` is the end of the log line "mem: lazy write-back on|off: ".

### `wb_lazy_choice()`

`set`: the value the player gave `NX2SARIA_WB_LAZY` (the environment or the settings file), NULL or "" when they gave
none. `windows`: the game runs on Windows (`WBL_ON_WINDOWS`). `driver_id`: the device's `VkDriverId`.

## `runtime/freeze_hold.h`

When the freezing of idle guest regions waits, the part without the OS: `runtime/vk/vk_memory.c` `freeze_pass()` makes
the freezes (`runtime/memory.c` `mem_freeze()`) and asks these functions whether a pass may run;
`tests/freeze_hold_check.py` runs this file alone.

A freeze is a VirtualQuery and a VirtualProtect of 2 MiB, some microseconds as a rule, and it only saves later
write-watch queries (about a microsecond per MiB per submission), so it can wait. Around an area load it is slow: in the
second after it, while the loaders write and the driver imports and locks new memory, freezes took 4 to 32 ms. Two rules
hold the passes back:

- A freeze of `FRZ_SLOW_NS` or more holds them for `FRZ_BACKOFF_NS`, doubled for each further slow one up to
  `FRZ_BACKOFF_MAX_NS`. The doubling starts over once `quiet_ns` pass without a slow freeze (`quiet_ns` 0: at the first
  fast freeze, the rule of 0.10.1, under which a slow freeze came every 250 ms for seconds after a load, fast ones
  between them).
- While the texture pool makes an area's textures, and for `load_hold_ns` after that or after an area's load, no pass
  runs (`load_hold_ns` 0: no such hold).

`FrzHold`: `hold_until`, the backoff holds the passes until then; `backoff`, the last hold's length, 0 for none since
the doubling started over; `last_slow`, when the last slow freeze ended, 0 for none yet; `load_until`, an area load
holds the passes until then.

### `frz_freeze_done()`

A freeze ended at `t1` after taking `took` ns.

### `frz_load_hold()`

Whether an area load holds the passes at `now`: `loading`, the texture pool is making an area's textures; `area_at`,
when the last area load began (0: none).

### `frz_may_run()`

Whether a pass may run at `now` (the backoff, then the load).

## `runtime/mem_need.h`

Whether the machine has the memory the game needs, asked once at the start (`runtime/memory.c` `mem_start_note()`). It
is a header of its own so that `tests/mem_need_check.c` checks it alone.

What counts is commit: memory the system has promised to programs, out of RAM plus page file (on Linux the tightest of
the limits `runtime/platform/platform.h` `plat_commit_info()` reads). An allocation fails when the commit in use reaches
the limit, however much RAM is free, and on Windows the video driver charges the game's video memory to it too. Measured
in play (RTX 5090, 3840x2160, three sessions of 19,000 to 50,000 frames on 2026-11-11): this process held 23,910 to
24,429 MB of commit, flat over the session, and the system stood at 65,777 to 65,914 of 73,154 MB with the other
programs' 41,500 MB. A machine that cannot give the game that much pages, drops textures to make room
(`runtime/vk/vk_texture.c` `commit_is_low()`) or loses the device.

`MEM_NEED_DEFAULT_MB`, 24576.0: the default of `NX2SARIA_MEM_NEED_MB`: the commit the game took in the sessions above,
as 24 GiB.

### `mem_need_room()`

The commit in MB the game can still get: what is free, and what this process already holds of its need.

### `mem_need_short()`

1 when the game's need does not fit. `need_mb` 0 or less: not asked. A limit below 0 is an unknown one: not short.

## `runtime/par_jobs.h`

`runtime/par_jobs.c`: a small pool of persistent threads for CPU work the drawing threads would otherwise do alone, such
as a large texture's copy out of the game's memory (`runtime/gfx/tex_deswizzle.c` `tex_bl_copy_mt()`). One job at a
time; the caller takes part and returns when every piece is done.

- `par_run()`: runs `fn(arg, i)` for every i in [0, n) on the pool's threads and the caller's, in no set order, and
  returns once all have returned. A second caller while the pool runs a job, a pool that could not start,
  `NX2SARIA_PAR_JOBS=0` or `n` 1 run every piece on the calling thread instead. `fn` must not call `par_run()` itself
  (it would run its pieces alone).
- `par_threads()`: the threads a job can spread over, the caller counted; 1 when the pool is off.
- `par_memcpy()`: memcpy with large copies (`PAR_MEMCPY_MIN` and up) split over the pool.

## `runtime/par_jobs.c`

A pool of persistent threads that runs one job at a time (`runtime/par_jobs.h`). The threads sleep on a condition
variable between jobs and cost nothing while the game does not use them. A job is `n` pieces claimed one by one from an
atomic counter by the caller and the workers alike, so a slow piece or a busy core evens out. One caller at a time: the
pool's busy lock is only tried, never waited for, so a second caller (or a piece that calls `par_run()`) runs alone
rather than waiting, and no caller can wait for a pool that waits for it.

Size: the usable hardware threads less two (the drawing thread and the game's main thread), at most `PAR_MAX_THREADS` in
all, the caller counted. `NX2SARIA_PAR_JOBS=0` runs every job on its caller (`runtime/cfg/cfg.c`).

- `PAR_MEMCPY_MIN`, 1 << 20: below this, waking the pool costs about what the copy does.
- The state `J`: `busy` is held by the caller for its whole job; `threads` is the workers, the caller not counted; `gen`
  is bumped per job, and a worker takes part in each job once; `active` is the workers that have not finished the
  current job; `fn` is the current job (written under `lock` before `gen` is bumped); `next` is the first piece not yet
  claimed.

## `runtime/part_file.h`

A file written piece by piece on a thread nobody waits for when the process ends.

The window's close ends the process with `plat_exit()` while the detached threads still run (`runtime/gfx/gfx_present.c`
`gfx_window_closed()`), and a kill or a crash ends it anywhere. A file such a thread was in the middle of then stays cut
short under its own name, and whoever opens it later takes it for a whole one. So the file is written as `<path>.part`
and gets its name with one rename once it is complete: a reader finds the whole file or none, and a `.part` file says
what it is. A file made in memory first goes through `plat_file_write_cached()` or `plat_file_write_atomic()` instead,
which do the same in one call.

### `part_file_name()`

`<path>.part` into `out`; 0 when it does not fit.

### `part_file_open()`

Opens `<path>.part` for writing (`mode`: "w" or "wb"). NULL when it cannot be made.

### `part_file_close()`

Closes `f`, the file `part_file_open(path)` gave, and renames it to `path`, over an older file of that name. With `ok`
0, after a write error on `f` or when the close or the rename fails, the part is deleted and `path` stays as it was. 1
when `path` now holds the whole new file.

## `runtime/sha256.h`

SHA-256 (FIPS 180-4), streaming. It uses the x86 SHA extensions when the CPU has them, portable C otherwise.

- `Sha256`: `bytes` is the message bytes so far; `buf[64]` a partial block.
- `sha256()`: init, update and final in one call.
- `sha256_ni()`: 1 when the hashes run on the x86 SHA extensions (SHA-NI), 0 when they run in portable C.
- `hmac_sha256()`: HMAC-SHA256 (RFC 2104) of `msg` under `key`, any key length (a key over 64 bytes is hashed first).
  `out` may be the same buffer as `msg`.

## `runtime/sha256.c`

SHA-256 (FIPS 180-4). The block function has two forms: one on the x86 SHA extensions (SHA-NI: Intel since Ice Lake and
Goldmont, AMD since Zen), which hashes the RomFS image at about the speed of a disk read, and portable C for CPUs
without them. `tests/sha256_paths_check.py` checks both against the FIPS 180-4 vectors and against each other. The file
is the project's own code and carries no licence notice.

### `blocks_ni()`

The state lives in two registers as the SHA instructions want it: ABEF and CDGH. Each group of four rounds adds the
round constants to four message words and runs `sha256rnds2` twice; the message schedule for words 16..63 is
`msg2(msg1(W[t-16..], W[t-12..]) + W[t-7..], W[t-4..])`.

Loading the state: the shuffle 0xb1 of `h[0..3]` gives CDAB, the shuffle 0x1b of `h[4..7]` gives EFGH, the align by 8
gives ABEF and the blend 0xf0 gives CDGH. Storing it back: the shuffle 0x1b of ABEF gives FEBA, the shuffle 0xb1 of CDGH
gives DCHG, the blend 0xf0 gives DCBA for `h[0..3]` and the align by 8 gives HGFE for `h[4..7]`.

### `cpu_has_sha_ni()`, `sha256_ni()`

`blocks_ni()` runs where CPUID reports the SHA extensions (leaf 7, subleaf 0, EBX bit 29), SSE4.1 (leaf 1, ECX bit 19)
and SSSE3 (leaf 1, ECX bit 9: the byte shuffle and the byte align). CPUID is read directly rather than through
`__builtin_cpu_supports("sha")`: clang before 19 rejects that feature name and stops the compile (Ubuntu 24.04's clang
18). `g_ni` is -1 until the first hash asks; `tests/sha256_paths_check.c` sets it to run one form or the other.

A CPU without leaf 7 has no SHA either. Threads that race at the first ask all store the same answer. Off x86-64 the
portable C form is the only one.

### `hmac_sha256()`

SHA-256((K ^ opad) || SHA-256((K ^ ipad) || msg)), K the key padded with zeros to the 64-byte block. The padded key and
the inner hash are wiped afterwards: they are key material (`runtime/hle/amiibo_crypto.c` derives its keys with this).

## `runtime/aes128.h`

AES-128 encryption (FIPS-197) and counter mode (SP 800-38A) with a 128-bit big-endian counter. Portable C; the amiibo
tag cipher (`runtime/hle/amiibo_crypto.c`) is its only user. Decryption is not needed: counter mode only encrypts.

- `Aes128.rk[176]`: the 11 round keys, 16 bytes each.
- `aes128_encrypt_block()`: `in` and `out` may be the same.
- `aes128_ctr()`: `out` = `in` XOR the key stream of the counter blocks iv, iv + 1, ... (the whole 16 bytes as one
  big-endian number, wrapping at 2^128). `n` need not be a multiple of 16; `in` and `out` may be the same buffer.

## `runtime/aes128.c`

AES-128 encryption on bytes: the state is 16 bytes in column order (byte r + 4c is row r of column c), as the standard
lays it out. The S-box is a table indexed by secret bytes, so the timing depends on the key and the data; that is
acceptable here because the only data is the player's own amiibo dumps, read locally. The file is the project's own code
and carries no licence notice.

### `xtime()`

Multiplication by x (that is 2) in GF(2^8) modulo x^8 + x^4 + x^3 + x + 1 (the 0x1b).

### `aes128_init()`

The key schedule, bytes 16 to 175: each 4-byte word is the word 16 bytes back XOR the previous word; at the start of
each round key the previous word is first rotated by one byte, run through the S-box and its first byte XORed with the
round constant.

### `aes128_encrypt_block()`

SubBytes and ShiftRows together: row r moves r columns to the left. MixColumns (not in the last round, so only for
rounds below 10): b0 = 2a0 ^ 3a1 ^ a2 ^ a3 and its rotations, written as a0 ^ (a0 ^ a1 ^ a2 ^ a3) ^ 2(a0 ^ a1).

### `aes128_ctr()`

The counter goes up by 1, carrying from the last byte (index 15) towards the first.

## `runtime/fma.c`

Correctly rounded `fma`/`fmaf` for x86-64 builds without FMA3: the `-march=x86-64-v2` build
([building.md](../building.md)). The file carries a licence notice in its head, which stays in the code.

`runtime/simd.h` lowers AArch64 FMADD/FMSUB and the RECPS/RSQRTS helpers to `__builtin_elementwise_fma`, which clang
turns into a call to `fma`/`fmaf` when the target has no FMA instruction. The C runtime is not a substitute: on CPUs
without FMA3 the UCRT's software fallback is documented as rounding incorrectly by up to half an ulp, while the FP
lowering needs the single rounding the ARM instruction gives. This file supplies it.

The result is rounded in the MXCSR rounding mode the caller set (`runtime/simd.h` `simd_rm_enter()`), and MXCSR FTZ and
DAZ are honoured as the hardware FMA would, so the fast paths of `runtime/simd.h` keep their assumptions: denormal
inputs read as zero and denormal results as zero under FPCR.FZ, without a trip through the slow path.

The definitions exist only where the compiler would emit the calls. A `-march=x86-64-v3` build defines `__FMA__` and
emits vfmadd instead, so this file contributes nothing there.

### `fmaf()`

Float: the exact product fits in a double, so one extra term and a round-to-odd step decide the float rounding. Halfway
and tiny results are the only ones where rounding the double first can change the float result. `xy` is exact. `r+t` is
the exact sum in nearest rounding; otherwise `t` keeps its sign and whether it is zero, which is all the round-to-odd
step needs. An odd sticky bit makes the final conversion round once, in whichever mode MXCSR asks for.

### `fma_scalbn()`

scalbn: the fma's final scaling, kept here so the result does not depend on the C runtime's version. Its multiplications
honour MXCSR like any other host FP operation. For a large negative `n` it makes sure the final n < -53 (the step
multiplies by 0x1p-1022 * 0x1p53), so the subnormal range rounds only once.

### `fma()`

Double: the significands are multiplied and aligned as integers, then converted back with one rounding.

- With `z` == 0 (its exponent above `ZEROINFNAN`) the result is `x * y`.
- The exponents are aligned: `z` is shifted left by `kz` and `r` right by `kr` with `kz + kr == d`.
- The top 63 bits are kept in `rhi`; the last bit is sticky. A zero result is `x * y + z`, an exact +-0.
- `r` converted to double has |r| in [0x1p62, 0x1p63].
- The result is subnormal before rounding when `e` is below -1022 - 62. At `e == -1022 - 63`, the smallest normal after
  rounding: whether it underflows depends on the architecture, and a double to float conversion (with 0x0.ffffff8p-63)
  imitates the before-rounding rule the guest has.
- One bit is lost when scaled: another top bit is added so the conversion rounds only once if inexact (the test is `rhi
  << 53`). Underflow is raised portably, so the compiler cannot optimize the expression away.
- Otherwise `d` is 10, so that scaling rounds only once.

## `runtime/lib_origin.h`

The game log's "libs:" line (`runtime/main.c` `log_libs()`), near its top: the form SHA-256 runs in, the SDL the process
loaded and which file it came from: one beside the game (the runpath `$ORIGIN` takes it ahead of the system's) or
another, so the log of a player's session says which one their game runs with.

`lib_origin_line()`: the line without its "libs: " prefix into `out`. `loaded` and `built`: `SDL_VERSIONNUM` values (the
library's `SDL_GetVersion`, the headers' `SDL_VERSION`); `sdl_path`: the loaded library's file (`plat_library_path()`),
"" when it was not found among the loaded modules; `exe_path`: the running executable (`plat_exe_path()`), "" when
unknown; `sha_ni`: `sha256_ni()`. Returns the whole line's length as snprintf counts it (`out` holds at most `cap - 1`
of it), -1 on an encoding error.

## `runtime/lib_origin.c`

The game log's "libs:" line (`runtime/lib_origin.h`). Only formatting: `runtime/main.c` `log_libs()` gathers the
versions and paths.

### `dir_len()`

The length of `path` up to its last separator, -1 without one. Windows paths take either separator.

### `same_dir()`

Whether the two files sit in one folder. Windows compares without case, as its file system does.

### `lib_origin_line()`

`SDL_VERSIONNUM` is major * 1000000 + minor * 1000 + micro. The path goes last: a log line that is too long loses its
end (`runtime/main.c` `rt_log()`), and the path is the only part of unbounded length.

## `runtime/utf16.h`

UTF-8 to UTF-16 conversion and back for the strings the guest keeps in UTF-16 (software keyboard, probes). Portable C,
no OS calls; malformed UTF-8 bytes and unpaired surrogates become U+FFFD.

### `utf8_to_utf16()`

`utf8` into `out` as NUL-terminated UTF-16, at most `cap` units with the NUL; returns the units written before the NUL.

### `utf16_to_utf8()`

NUL-terminated (or `n` units, whichever comes first) UTF-16 into `out` as NUL-terminated UTF-8, at most `cap` bytes with
the NUL; returns the bytes written before the NUL.

## `runtime/nx2saria.rc`

The resource script: the application manifest of `nx2saria.exe` (`runtime/nx2saria.manifest`) and its icon
(`runtime/nx2saria.ico`, also the icon of `NX2Saria-Setup.exe` and of the installer window). `tools/build_game.py`
compiles it with the runtime objects, so it is also a member of `libnx2navirt.a`, which the release links with
`--whole-archive`.

## `runtime/nx2saria.manifest`

Embedded by `runtime/nx2saria.rc`. The comment check does not read XML; the file's one comment moved here by the same
rule.

- `activeCodePage` makes UTF-8 the process code page (Windows 10 1903 and later): the narrow file functions (fopen,
  CreateFileA, GetModuleFileNameA) and getenv then carry UTF-8, the same encoding `runtime/platform/win32.c` and
  `runtime/hle/fs_host.c` convert with, so an install folder or user profile with a non-ASCII name works.
- `dpiAwareness` PerMonitorV2 (`dpiAware` true/pm on Windows 10 before 1607): window, monitor and cursor sizes are
  physical pixels and the swapchain matches the screen. A DPI-unaware process gets them divided by the display scale and
  its image stretched back up by Windows, so 4K at 150% presented a 2560x1440 image, blurred.
  `runtime/gfx/gfx_window_win.c` follows a change of the window's DPI (WM_GETDPISCALEDSIZE, WM_DPICHANGED).
- `longPathAware` lets the wide file functions of `runtime/platform/win32.c` take paths longer than 260 characters (a
  mod's files deep in a long game folder) where Windows has long paths switched on (LongPathsEnabled); elsewhere it does
  nothing.

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

The room the card and the system have, as a level per side (`runtime/mem_budget.h`): sampled every
`MEM_BUDGET_PERIOD_MS` on the "mem budget" thread, logged when a level changes, read by the owners of memory that can be
given back (the "SPIR-V at rest" part of `runtime/gfx/gfx_shader.c` compresses sooner when the system is tight) and
shown in the memory report (`runtime/vk/vk_memory.c` `system_log()`). A sample is one driver query (`vk_mem_vram()`) and
two system calls (`plat_commit_info()`, `plat_mem_info()`): well under a millisecond, never on a drawing thread.

The state `B`: `vram_lock` is held over a video memory sample, and `mem_budget_vram(0)` waits for it; `vram_on` says the
device is up (under `vram_lock`); `test_mb[]` holds `MEM_BUDGET_TEST_VRAM_MB` and `MEM_BUDGET_TEST_RAM_MB`, 0 off.

### `mem_budget_vram_mb()`

The use and the budget of the last video memory sample, the figures its level was made from; 0 before the first
sample. Read by the settings menu for the line of the *Apply your changes?* box.

### `sample_vram()`

Also gives the use and the budget in MiB as it measured them, for `runtime/vram_warn.c` `vram_warn_sample()`: the
figures the notice logs are the ones the level was made from, `NX2SARIA_MEM_BUDGET_TEST_VRAM_MB` included.

The video memory's room is the driver's budget (at most `MEM_BUDGET_TEST_VRAM_MB`) less this process's use. It returns 0
while the device is not up or the driver gives no figure.

### `sample_ram()`

The system's room: the lower of the commit's headroom and the physical memory free; with `MEM_BUDGET_TEST_RAM_MB` that
many MiB less this process's commit.

### `budget_main()`

After each video memory sample the level goes to `vram_warn_sample()`: critical or not, with the sample's figures.

The first sample of a side gives its level as it is, without the hysteresis: `mem_level_next()` starts from `MEM_ROOMY`.

## Forcing the condition on a large card

`NX2SARIA_MEM_BUDGET_TEST_VRAM_MB=<MiB>` (`runtime/mem_budget.h`) makes the level take the video memory for that
small: set below what the game uses (2048 is enough at any resolution), the level is critical from the first sample
and the notice comes at the tenth sample, 18 s later. The key changes the levels only, never the texture cache's own rules, so the game
runs as before and only the notice and its log lines are new. `NX2SARIA_VRAM_BUDGET_MB` and
`NX2SARIA_VRAM_BALLAST_MB` (`runtime/vk/vk_memory.c`) are the tests that make the renderer itself short of memory.
