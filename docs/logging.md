# The game log

The runtime writes one log for each run. Players attach it to issues, and it is how a change is shown to be on and
working in a normal session (AGENTS.md, "Every change leaves a trace in the log"). This page says how a line is written,
where it goes, how often a line may repeat, and which prefix each part of the runtime puts at the start of its lines.

## Where the log goes

- A game run whose stderr is not a file already (started from the launcher, Explorer, a shortcut or a console) writes
  `logs/nx2saria.log` next to the game executable, and stderr too. `tools/run_game.py` and `2> file` keep their own
  file, and no `logs/nx2saria.log` is written (`runtime/main.c` `log_open`).
- Each start renames the previous logs: `nx2saria.log` becomes `nx2saria.1.log`, up to `nx2saria.4.log`, and the
  oldest is replaced (`LOG_KEEP` 5).
- A file stops at 64 MiB and says so in one line; the rest of the run goes to stderr only. The lines of the run's
  end (the exit summary, the frame time summary, a FATAL line, a crash report) still reach the file, up to 4 MiB more
  (`main.c` `log_sink`, `LOG_END_RESERVE`).
- Lines written before the file opens wait in a 32 KiB buffer and are written first.
- The account's folders and the computer's name are written short in every line ([log-privacy.md](log-privacy.md)).

## Writing a line

The functions are declared in `runtime/host.h` and `runtime/dispatch.h`.

| Function | Writes | Use it for |
|---|---|---|
| `rt_log(fmt, ...)` | `[host] ` + the formatted text + a newline, in one write | Every ordinary line. Start the text with the prefix of your part of the runtime (below). |
| `rt_logf(fmt, ...)` | The formatted text as it is: no `[host] `, no newline | A block of whole lines in one call: the boot steps, a dump of an IPC message. |
| `rt_vlogf(fmt, ap)` | As `rt_logf`, from a `va_list` | A function of your own that takes a format. |
| `rt_log_raw(p, n)` | `n` bytes as they are | Text that is not a format: the game's own debug output (`svc.c`), a line built with `runtime/log_line.h`. |
| `rt_fatal(fmt, ...)`, `rt_fatal_code(code, fmt, ...)` | `FATAL: ` + the text, then the notes (`fatal:` lines), and ends the process | An error the run cannot go on from. The launcher reads the FATAL line. |
| `rt_log_sync()` | Everything queued, now, and turns the queue off for the rest of the run | The end of the process, a fatal error, a crash report, a bug capture. Not for ordinary lines. |

A line is limited to 2 KiB (2048 bytes with the `[host] ` and the newline); a longer one is cut.

Each call is one write (a line with a personal folder in it, as long as its short form fits `rt_log_raw()`'s
2560-byte copy), and a write is never split by another thread's. A line written in several calls is not: another
thread's whole line can land between two of its pieces. A line made of pieces is built first with `runtime/log_line.h`
(`log_line_add()`, then `log_line_end()`) and written in one `rt_log_raw()`; one longer than its buffer comes out cut,
ending in ` [line cut]`. The crash report and the FATAL line of a fatal error are written this way.

Lines that only help while debugging one part go behind `NX2SARIA_TRACE` (`rt_trace & RT_TRACE_<X>`; the items are
`svc`, `host`, `ipc`, `fs`, `gfx`, `perf`, `sched`, `audio`, `present` and `all`), or behind a setting of their own,
so a normal session does not write them.

## The log thread

In a game run the lines are written by a thread of their own (`runtime/logq.c`), so a slow write (a console that
scrolls, a pipe whose reader is busy, a virus scanner) does not hold the render thread:

- `rt_log` copies the line into a 1 MiB queue under a lock held for the copy alone, and returns. Lines keep the order
  they were queued in.
- The log thread waits 250 ms after the first line it finds, or until the queue is half full, and writes everything
  queued by then in one write. The file and the console are up to 250 ms behind the game.
- A line that finds the queue full is written by its own thread, after everything queued before it.
- A write that took 2 ms or longer is counted; a `log:` line says how many, at most every ten seconds.
- `rt_log_sync` writes the queue on the calling thread and turns it off, so an exit, a fatal error and a crash report
  lose no line. A process ended from outside runs no code of ours and loses what was being gathered.
- `NX2SARIA_LOG_ASYNC=0` turns the thread off: each line is written by the thread that logs it. The start says which
  (`log: lines are written by a thread of their own` or `log: no log thread`).

## Lines that repeat

A line that can come every frame, every draw or every file must not fill the log: the 64 MiB cap is reached in about
90 minutes by a `perf` trace, and a player's log is read by a person. Each such line is limited per session or
aggregated. The ways the runtime does it:

- Once per session: a static flag beside the call. `vk_pipeline.c` and `vk_exec_xfer.c` each define a local
  `LOG_ONCE(...)` macro for it, `nvn_ext.c` an `ONCE(...)` with the `nvn: ` prefix; `host.h` has no shared one.
- A count with a cap: `wide_bars.c` `screen_log` (its last line says that later ones are not logged),
  `vk_texture.c` `log_limited` (16 lines), `vk_device.c` validation messages (500).
- A sum over a period, written as one line every N frames or seconds: the `perf:` windows, `frametime:`, the
  `fsr3fg:` and `fg:` periodic lines, `log:` slow writes.
- A summary at the end of the run: `frametime: exit`, the stutter map, `log: ... had a personal folder written short`.

A new line that repeats picks one of these and says in the line or its comment what its limit is.

## Prefixes

Every line starts with a prefix that names the part of the runtime that wrote it, so a reader can search a player's
log for one feature (`grep "frame rate:"`). The prefix is the text before the first colon that ends a word, when that
text is one to four words: `vk:`, `frame rate:`, `host save import:`, `aud:u:` (a service name with its own colon).

Use the prefix of the part you change; a new part takes a new prefix, adds its row below, and keeps it short and
lower case. `tests/log_prefixes_check.py` fails when a prefix in `runtime/` has no row here.

The prefixes grew one feature at a time and are kept as they are: players already quote them in issues, and the
launcher (the `FATAL: ` line, `launcher/ui/app.py`), `tools/run_game.py` (`boot map: arrived in`) and
`tools/stutter_map.py` (`stutter map`) read them. Renaming one is a change of its own, made together with its readers.

### Near-duplicates

Several groups of prefixes name the same subsystem with different words, or one subsystem and a part of it. They are
listed as they are; search for all of a group when you look at one subsystem:

- Vulkan: `vk:`, `vk` (validation messages), `vk caps:`, `vk selftest:`, `vk_dump:`, `vk_exec:`, `vk_gpa:`,
  `vk_hdr:` (and `hdr:` for the same HDR output), `vk_pipeline:`, `vk_rec:`, `vk_usnap:`, `texup:`, `gpu passes:`,
  `set-at:`, `SEQ`, `cs`, `evo:`, `mvbanks:`.
- Upscaling and frame generation: `dlaa:`, `dlaa survey:`, `dlaa times:`, `dlssg:`, `ngxfg:`, `fsr3:`, `fsr3fg:`,
  `ffx:`, `ffx4:`, `fg:`, `fg4:`, `aa:`, `lowlat:`, `motion policy:`, `selective motion:`, `motion direct:`,
  `motion ownership:`, `mv_check:`, `mv_check total:`, `mv_prev:`, `mv_survey:`.
- Errors: `FATAL:` is the line the launcher reads, `fatal:` the notes after it, `exit:` a fault at the end.
- Frame rate and timing: `frame rate:`, `frame step:`, `frame` (per-frame GPU counts), `frametime:`, `fps:`,
  `present:`, `perf:`, `segment:`, `scene:`, `stutter:`, `stutter map:`, `stutter` (the map's lines), `time`.
- Floors: `floor probe:`, `floor scan:`, `floor watch:`.
- Memory: `mem:`, `mem_budget:`, `vram_warn:`, `MapPhysicalMemory`, `UnMapPhysicalMemory`, `heap retry:`, `platform:`.
- RomFS and files: `RomFS:`, `RomFS` (the image and index checks), `RomFS overlay:`, `RomFS ready:`, `fs:`, `fs`
  (mounts and the RomFS trace), `fs commit save:`, `host save import:`, `host save recovery:`, `host save storage:`,
  `host save working copy:`, `save share:`, `mods:`.
- Menus: `menu:`, `menu_cursor:`, `menu_mouse:`, `menu_sound:`, `quit_menu:`, `pause_tab:`, `debug:`.
- Widescreen: `widescreen:`, `wide hud:`, `wide menus:`, `balloon:`, `camera clearance:`, `fov:`.
- Input and motion: `input:`, `hid:`, `motion:` (the gyro), `mouse:`, `mousetrace:`, `mic:`.
- Audio: `audio:`, `audren:`, `aud:u:`, `auddmg:u manager:`, `audsmx:u:`, `nn::codec:`.
- amiibo: `amiibo:`, `amiibo crypto selftest:`, `amiibo crypto selftest failed:`, `nfp:`.
- Boot and modules: `boot:`, `boot map:`, `loader:`, `loaded`, `libs:`, `patches:`, `version:`, `telemetry:`.
- Host and guest calls: `host calls:`, `host` (the trace of each call), `guest`, `svc`, `setjmp:`, `longjmp`,
  `ENTER`, `RETURN`, `CreateThread`, `AllocateAddressRegion`, `AllocateMemoryPages`.
- System services: `applet:`, `applet proxy:`, `appletOE:`, `oe:`, `ipc:`, `csrng:`, `aoc:`, `nifm:`, `pctl:`,
  `prepo:`, `pl:`, `err:`, `diag:`, `account:`, `vi:`, `nv:`.
- Compression: `zstd:`, `zstd check:`.

### Prefixes without a colon

20 prefixes have no colon: more words, a number or a parenthesis come between the first word and the first colon, or
the line has no colon at all. Most are debug traces. The table writes them as that first word, without a colon:
`AllocateAddressRegion`, `AllocateMemoryPages`, `CreateThread`, `cs`, `ENTER`, `frame`, `fs`, `guest`, `host`,
`loaded`, `longjmp`, `MapPhysicalMemory`, `RETURN`, `RomFS`, `SEQ`, `stutter`, `svc`, `time`, `UnMapPhysicalMemory`
and `vk`. Five of them share their word with a prefix that has a colon and names the same subsystem: `frame`
(`frame rate:`), `fs` (`fs:`), `RomFS` (`RomFS:`), `stutter` (`stutter:`) and `vk` (`vk:`).

### The table

Files are under `runtime/` unless stated otherwise; the first three by number of lines, `...` when there are more.

| Prefix | What writes it | Main files |
|---|---|---|
| `aa:` | Anti-aliasing choice: SMAA, TAA and how they pair with the upscalers | `vk_aa.c`, `vk_exec_draw.c` |
| `account:` | The local user account the game is given (ov_system) | `ov_system.c` |
| `action prompt:` | Action button prompts: face button layout | `action_prompt.c` |
| `AllocateAddressRegion` | Trace of a system memory call (ov_system), no colon | `ov_system.c` |
| `AllocateMemoryPages` | Trace of a system memory call (ov_system), no colon | `ov_system.c` |
| `amiibo:` | amiibo figures: keys, drops, the daily limit | `amiibo.c`, `amiibo_crypto.c`, `amiibo_daily.c` |
| `amiibo crypto selftest:` | amiibo key self-test | `amiibo_crypto.c` |
| `amiibo crypto selftest failed:` | amiibo key self-test failure | `amiibo_crypto.c` |
| `aoc:` | Add-on content service (none) | `ov_system.c` |
| `applet:` | Applet message queue | `ipc.c` |
| `applet proxy:` | Applet proxy IPC commands (NX2SARIA_TRACE=ipc) | `ipc.c` |
| `appletOE:` | appletOE IPC commands (NX2SARIA_TRACE=ipc) | `ipc.c` |
| `aud:u:` | Audio output service IPC commands (NX2SARIA_TRACE=ipc) | `ipc.c` |
| `auddmg:u manager:` | Audio device manager service: answers with no input devices | `audio_mixer.c` |
| `audio:` | Audio renderer and mixer: device, faults in what the game sends, one line per kind | `audio_mixer.c`, `audren.c` |
| `audren:` | Audio renderer updates and sizes | `audren.c` |
| `audsmx:u:` | Audio stream mixer service: refused input streams | `audio_mixer.c` |
| `balloon:` | Speech balloon widescreen patch | `widescreen.c` |
| `boot:` | Boot of the game modules: init, checkpoints, frames presented in a test boot | `boot.c` |
| `boot map:` | Boot to map: the area a run opens in (docs/boot-to-map.md) | `boot_map.c` |
| `button prompts:` | Button prompt art and its record (button-prompts.txt) | `romfs.c` |
| `camera clearance:` | Camera wall clearance on pictures wider than 16:9 | `camera_clearance.c` |
| `camera speed:` | Camera speed patch and the data it reads | `camera_speed.c` |
| `capture:` | F8 bug capture (docs/bug-capture.md) | `bugcap.c` |
| `cfg:` | Settings file and environment: keys read, unknown keys, replays of a capture | `cfg.c`, `cfg_support.c`, `perf.c`, ... |
| `changebank hold:` | Map bank changes held for the neighbour (docs/changebank-hold.md) | `changebank_hold.c` |
| `cheats:` | Cheats of the mods' cheat files: how many and how many on at start and whether the menu shows its Cheats tab, each cheat switched on or off, code cheats and ExeFS patch files not built in, and per data cheat its writes: `<mod>/<name>: N writes in N frames, N unchanged, N refused so far` at its first write and at most once a minute while it keeps writing, `... this session` at the end | `cheats.c` |
| `console:` | Console window closed or Ctrl+C | `main.c` |
| `core-diag:` | CPU core diagnostics (NX2SARIA_CORE_DIAG) | `kernel.c` |
| `cpu:` | CPU cores and logical processors at start | `perf.c` |
| `CreateThread` | Trace of a guest thread start (kernel), no colon | `kernel.c` |
| `crit:` | Recording of the scheduler's critical waits to crit.bin (kernel) | `kernel.c` |
| `cs` | Compute shader bindings, a debug line (vk_exec_draw), no colon | `vk_exec_draw.c` |
| `csrng:` | Random number service commands | `ipc.c` |
| `debug:` | Debug menu and flags (F9 with debug_menu = 1) | `debug_tools.c`, `ui/debug_menu.cpp` |
| `diag:` | Host diagnostics and crash reporter notes | `ov_system.c`, `perf.c` |
| `discord:` | Discord Rich Presence | `discord_rpc.c` |
| `dlaa:` | DLSS/DLAA upscaling, motion vectors and the frame generation inputs | `vk_dlaa.c` |
| `dlaa survey:` | Depth test survey of scene draws for DLAA | `vk_dlaa.c` |
| `dlaa times:` | GPU times of the DLAA passes | `vk_dlaa.c` |
| `dlssg:` | DLSS Frame Generation through Streamline | `vk_dlssg.c` |
| `ENTER` | Trace of a host override being entered (ov_system), no colon | `ov_system.c` |
| `worker:` | Persistent launcher worker: startup, runtime and active app paths (shortened), lock, waiting update, inspection, first-run preferences and requirements, installed settings and Discord activity writes, udmabuf offer outcomes, interface language read and write outcomes, uninstall scheduling and refusal, appearance settings and background reads and writes, and folder open results, destination and dump proposals, disk-space and path-limit checks, dump search counts and duration, install and rebuild request decisions, build supervisor readiness and identity mode, interrupted build classification, wait and stop results, reboot and player-confirmed recovery, Play, memory, spawn, first game window focus, first restart observed, hide preference save failures, manual and automatic update check start and result, save export, import and preferences, mod list counts and action outcomes, Steam detection and shortcut write results, disconnect reason, deferred crash-offer load failures and protocol outcomes | `launcher/worker.py` (outside `runtime/`) |
| `electron:` | Development Electron launcher in `launcher-electron.log`: build hash, process ID, selection origin, brand and worker version, inspection with its source and pending rebuild, install, rebuild and interrupted-build recovery requests and outcomes, first-run and installed preference save outcomes, udmabuf confirmation and outcome, hide and appearance preferences, appearance asset counts, first standard controller use, interface language choice and save result, dump and log-folder results, manual and automatic update check results, save export, import, mod and Steam outcomes, uninstall decisions and results, reports question, consent, preview and comment outcomes, crash offer and decisions, preferences, Play results, game window ready and exit codes, window visibility, lock refusal and the displayed game-end message kind, launcher exit; no paths | `launcher-electron/src/dev-log.ts` (outside `runtime/`) |
| `err:` | Game error dialog | `ov_system.c` |
| `evo:` | Changes of draw order over frames (vk_exec, debug) | `vk_exec.c` |
| `exit:` | A fault while the process was ending | `main.c` |
| `FATAL:` | The fatal error or crash line itself (the launcher reads it); rt_fatal writes it too | `main.c`, `svc.c` |
| `fatal:` | Notes after a FATAL line: time, memory commit, causes | `dispatch.c`, `kernel.c`, `main.c`, ... |
| `ffx:` | AMD FidelityFX SDK contexts | `vk_ffx.cpp` |
| `ffx4:` | AMD FidelityFX SDK 4 contexts | `vk_ffx4.cpp` |
| `fg:` | FSR SDK frame generation and its pacing | `vk_fg.cpp`, `vk_fg_time.c` |
| `fg4:` | FSR SDK 4 frame generation | `vk_fg4.cpp` |
| `flight:` | Flight recorder of recent events | `flight.c` |
| `floor probe:` | Floor probe debug lines | `frame_rate.c` |
| `floor scan:` | Floor scan snapshots | `frame_rate.c` |
| `floor watch:` | Floor watch debug tool | `floor_watch.c` |
| `folder_pick:` | The folder dialog | `folder_pick_win.cpp`, `folder_pick_sdl.c` |
| `foliage:` | The foliage's draw distances (`foliage_distance`) at start | `foliage_distance.c` |
| `fov:` | Field of view lens changes | `debug_tools.c`, `resolution.c` |
| `fps:` | Frames and game time over a period | `frame_rate.c` |
| `frame` | Per-frame GPU work counts (vk_exec trace), no colon | `vk_exec.c` |
| `frame rate:` | Frame rate and pacing: the 60 fps patches, frame limit, RivaTuner limit | `rtss_limit.c`, `frame_rate.c`, `animation_loop.c`, ... |
| `frame step:` | The step Ocarina is given each frame | `frame_rate.c` |
| `frametime:` | Frame time summary: windows, exit, waits | `perf.c` |
| `fs` | File system mounts and the RomFS trace, no colon | `fs_host.c`, `romfs.c` |
| `fs:` | File system: moves, failures, the save folder | `fs_host.c`, `romfs.c` |
| `fs commit save:` | Save commits | `fs_host.c` |
| `fsr3:` | AMD FSR 3.1 upscaling (the runtime's passes) | `vk_fsr3.c` |
| `fsr3fg:` | AMD FSR 3.1 frame generation and its presenter | `vk_fsr3fg.c`, `gfx_present.c` |
| `gfx:` | Window, display mode, focus, present path | `gfx_present.c`, `gfx_window_sdl.c`, `gfx_window_win.c`, ... |
| `gfx_shader:` | Shader translation, the shader cache and its packs | `gfx_shader.c` |
| `gpu passes:` | GPU time by pass (vk_crumbs) | `vk_crumbs.c` |
| `guest` | The game's own Break, abort and ExitProcess (svc), no colon | `svc.c` |
| `hang watch:` | Hang watch (NX2SARIA_HANG_WATCH_S) | `hang_watch.c` |
| `hdr:` | HDR output and the game's tonemap | `vk_hdr.c` |
| `heap retry:` | Retry of the game's heap allocations (docs/heap-retry.md) | `heap_retry.c` |
| `hid:` | Controllers as the game sees them (HID), rumble | `ov_input.c` |
| `host` | Trace of host function calls (NX2SARIA_TRACE=host), no colon | `hostfn.c` |
| `host calls:` | Host function call counts | `hostfn.c` |
| `host save import:` | Save import from a shared zip at the next start | `fs_host.c` |
| `host save recovery:` | Save recovery at start | `fs_host.c` |
| `host save storage:` | Where the save lives | `fs_host.c` |
| `host save working copy:` | The save working copy | `fs_host.c` |
| `input:` | Input: controllers, bindings, input scripts | `input.c`, `input_ds3_win.c`, `input_map.c`, ... |
| `ipc:` | IPC sessions and commands (NX2SARIA_TRACE=ipc) | `ipc.c` |
| `kernel:` | Kernel emulation: handles, waits, threads | `kernel.c` |
| `libs:` | Libraries loaded into the process | `main.c` |
| `loaded` | Each module loaded (loader), no colon | `loader.c` |
| `loader:` | Module loader: relocations, imports | `loader.c` |
| `log:` | The log itself: path, log thread, slow writes, personal folders written short, a stderr lock given up at the end | `main.c`, `log_privacy.c`, `logq.c` |
| `longjmp` | Guest longjmp debug lines, no colon | `guest_setjmp.c` |
| `lowlat:` | Low latency mode (Reflex, anti-lag) | `vk_lowlat.c` |
| `MapPhysicalMemory` | A failed physical memory map (memory), no colon | `memory.c` |
| `mem:` | Memory: guest heap, video memory, budgets, freezes | `vk_memory.c`, `memory.c`, `vk_texture.c`, ... |
| `mem_budget:` | Memory budget levels | `mem_budget.c` |
| `menu:` | In-game menu: options, applies, art | `ui/menu.cpp`, `ui/option_rows.cpp`, `ui/menu_art.cpp`, ... |
| `menu_cursor:` | Menu cursor screens and hit panes | `menu_cursor.c` |
| `menu_mouse:` | Mouse in the menus (docs/menu-mouse.md) | `menu_mouse.c` |
| `menu_sound:` | Menu sounds | `menu_sound.c` |
| `mic:` | Microphone for the ocarina (docs/audio-input.md) | `ov_audio_input.c` |
| `mods:` | Mods folder | `mods.c`, `romfs.c` |
| `motion:` | Gyro motion settings and calibration (docs/gyro.md) | `input.c`, `motion_fusion.c` |
| `motion direct:` | Motion vectors: direct shape bindings | `motion_owner.c` |
| `motion ownership:` | Motion vectors: model ownership | `motion_owner.c` |
| `motion policy:` | Frame generation motion guard | `vk_fg_pose.inc` |
| `mouse:` | Direct mouse camera input | `mouse_direct.c` |
| `mousetrace:` | Mouse input trace | `mouse_direct.c`, `vk_dlaa.c` |
| `movie:` | Movie playback (VP9) | `ov_movie2.c`, `ov_movie.c` |
| `mv_check:` | Motion vector check | `vk_mvcheck.c` |
| `mv_check total:` | Motion vector check totals | `vk_mvcheck.c` |
| `mv_prev:` | Previous-frame motion vector pass times | `vk_mvprev.c` |
| `mv_survey:` | Motion vector survey of uniform blocks | `vk_mvsurvey.c` |
| `mvbanks:` | Motion vector uniform banks (debug) | `vk_exec_draw.c` |
| `net:` | Network sockets (offline) | `net_offline.c` |
| `nfp:` | NFC (amiibo) service sessions | `nfp.c` |
| `ngxfg:` | NVIDIA NGX frame generation | `vk_dlaa.c` |
| `nifm:` | Network interface service (offline) | `ov_system.c` |
| `nn::codec:` | Opus audio decoding | `ov_media.c` |
| `nv:` | Graphics driver initialisation (nv::) | `ov_gfx.c` |
| `nvn:` | NVN graphics API calls the runtime does not implement or traces (NX2SARIA_TRACE=gfx) | `nvn_api_core.c`, `nvn_api_cmd.c`, `nvn_api_obj.c`, ... |
| `NX2SARIA_TRACE:` | An unknown NX2SARIA_TRACE item | `main.c` |
| `ocarina:` | Ocarina recorder clock and staff | `frame_rate.c` |
| `oe:` | Operation mode and performance requests the game sees | `ov_system.c` |
| `par_jobs:` | Parallel jobs for large texture copies | `par_jobs.c` |
| `pause_tab:` | The settings tab in the game's pause menu (docs/pause-settings-tab.md) | `pause_tab.c` |
| `patches:` | The patch sets and whether each is on, at start | `patches.c` |
| `pctl:` | Parental controls service (none) | `ov_system.c` |
| `perf:` | Performance windows: waits, bound verdicts, counters | `vk_exec.c`, `vk_texture.c`, `vk_sync.c`, ... |
| `perfmon:` | GPU monitoring through NVML | `perfmon.c` |
| `pl:` | Shared fonts | `ov_system.c` |
| `platform:` | Memory commit note from the platform layer | `vk_texture.c` |
| `power:` | Power and timer settings | `main.c` |
| `prepo:` | Play reports (kept local) | `ov_system.c` |
| `probe:` | The lines the busy crash probes' threads log while the main thread fails (`--probe-crash-busy`) | `main.c` |
| `present:` | Present intervals (NX2SARIA_TRACE=present) | `vk_backend.c` |
| `preset:` | Graphics presets and the hardware detection behind them | `cfg.c`, `hw_detect.c` |
| `prof:` | Sampling profiler (NX2SARIA_PROFILE) | `prof_win.c`, `perf.c` |
| `quit_menu:` | Quit Game in the game's own menus (docs/quit-menu.md) | `quit_menu.c` |
| `rdoc:` | RenderDoc capture (NX2SARIA_RDOC) | `rdoc.c` |
| `resolution:` | Resolution and dynamic resolution | `resolution.c`, `dynres.c` |
| `restart:` | Restart wait for the previous process | `main.c` |
| `RETURN` | Trace of a host override returning (ov_system), no colon | `ov_system.c` |
| `RomFS` | RomFS image and index checks, no colon | `romfs.c` |
| `RomFS:` | RomFS reads and checks | `romfs.c` |
| `RomFS overlay:` | RomFS overlays and their manifests | `romfs.c` |
| `RomFS ready:` | RomFS ready, with counts and time | `romfs.c` |
| `save share:` | Shared save export and import | `save_share.c` |
| `scene:` | Scene loads, with frame and world time | `perf.c` |
| `sched:` | Scheduler periods (NX2SARIA_TRACE=sched) | `kernel.c` |
| `segment:` | Performance segments | `perf.c` |
| `selective motion:` | Selective motion vectors for frame generation | `vk_fg_pose.inc` |
| `SEQ` | Depth clear, copy and present sequence (vk_exec debug), no colon | `vk_exec_xfer.c`, `vk_exec.c` |
| `set-at:` | Settings changed at a frame (vk_crumbs) | `vk_crumbs.c` |
| `setjmp:` | Guest setjmp buffers | `guest_setjmp.c` |
| `sprint:` | Sprint button | `sprint.c` |
| `stutter` | Stutter map report lines, no colon (the head is "stutter map (...)") | `stutter_map.c` |
| `stutter:` | One stutter episode | `stutter_map.c` |
| `stutter map:` | Stutter map report | `stutter_map.c`, `perf.c` |
| `svc` | Trace of supervisor calls (NX2SARIA_TRACE=svc), no colon | `svc.c` |
| `swkbd:` | Software keyboard | `swkbd.c`, `swkbd_sdl.c` |
| `telemetry:` | The session summary for the launcher's session reports ([telemetry.md](telemetry.md)): where it goes, at start, and that it was written, at the end | `session_summary.c` |
| `tex_astc:` | ASTC decode workers | `tex_astc_mt.c` |
| `text speed:` | Text speed setting | `text_speed.c` |
| `texup:` | Texture upload work per frame | `vk_texture.c` |
| `time` | Time dump debug lines (frame_rate), no colon | `frame_rate.c` |
| `tracker api:` | Tracker API clients (docs/tracker-api.md) | `tracker_api.c` |
| `tsan:` | Thread sanitizer race probe | `main.c` |
| `UnMapPhysicalMemory` | A failed physical memory unmap (memory), no colon | `memory.c` |
| `version:` | Product and game version, at start | `main.c` |
| `vi:` | Display service: HDR status and metadata, refresh rate | `ov_gfx.c` |
| `vk` | Vulkan validation layer messages, no colon | `vk_device.c` |
| `vk:` | Vulkan back end: device, memory, textures, passes, presents | `vk_texture.c`, `vk_exec_draw.c`, `vk_device.c`, ... |
| `vk caps:` | Vulkan capabilities the runtime chose | `vk_caps.c` |
| `vk selftest:` | Vulkan self-test | `vk_device.c` |
| `vk_dump:` | Vulkan dumps for debugging | `vk_dump.c` |
| `vk_exec:` | Command execution: draws, passes, mesh emulation | `vk_exec_draw.c`, `vk_tonemap.c`, `vk_exec.c`, ... |
| `vk_gpa:` | GPU address table | `vk_gpa.c` |
| `vk_hdr:` | HDR encode resources | `vk_hdr.c` |
| `vk_pipeline:` | Vulkan pipelines: building, caches, formats | `vk_pipeline.c` |
| `vk_rec:` | Vulkan recorder thread | `vk_rec.c` |
| `vk_usnap:` | Uniform snapshots of a submission | `vk_usnap.c` |
| `vram_warn:` | The on-screen notice of full video memory: on or off at start, each notice with its numbers | `vram_warn.c` |
| `voice language:` | Voice language option | `voice_language.c` |
| `wide hud:` | Widescreen HUD layout (docs/wide-hud.md) | `wide_hud.c`, `romfs.c` |
| `wide menus:` | Widescreen menus | `wide_hud.c`, `romfs.c` |
| `widescreen:` | Widescreen: camera lens, bars, menus | `widescreen.c`, `wide_bars.c`, `debug_tools.c` |
| `zstd:` | Host zstd decoding with the game's dictionaries | `ov_zstd.c` |
| `zstd check:` | Host zstd decoder against the guest | `ov_zstd.c` |

## Prefixes of lines the check cannot see

Some lines are made in a buffer by another function, or take their first word from a variable, and `rt_log` is given
only `"%s"` or `"%s: ..."`. The check reads such a buffer when the same function filled it with `snprintf`, `sprintf`
or an assignment of a string literal; it cannot see the ones below (`python tests/log_prefixes_check.py --unseen` lists
the calls). Their prefixes, most of which the main table has too:

| Prefix | Where the text is made | Files |
|---|---|---|
| `resolution:` | The callers of `state_log`, which build the dynamic resolution state lines | `dynres.c` |
| `fsr3fg:`, `ngxfg:` | `ng_name`: the frame generation presenter's lines name the generator in use | `gfx_present.c` |
| `fg:`, `fg4:` | `fgs_line` (`vk_fg_stats.h`), with the name the caller passes: the periodic frame generation line | `vk_fg.cpp`, `vk_fg4.cpp`, `vk_fsr3fg.c` |
| `dlssg:` | The DLSS-G library (`runtime/dlssg/nx2_sl.cpp` `say`), handed to the log by `sl_line` | `vk_dlssg.c` |
| `kernel:` | `wait_pass`: the guest threads' long lock and thread-end waits, for a hang report | `kernel.c` |
| `motion:` | `ADD_LINE` in `motion_fusion.c`: the gyro calibration lines | `motion_fusion.c` |
| `stutter:` | One stutter episode of a single frame (`c->line`) | `stutter_map.c` |
| `vk:` | `vk_crumbs.c` `log_lines` (pass marks), `gpu_choice.h` (the card chosen) and `nvidia_cache.h` (the NVIDIA shader cache folder) lines | `vk_crumbs.c`, `vk_device.c` |
| `mem:` | `mg_put` (`mem_growth.h`): the memory report lines | `vk_memory.c` |
| `wide hud:`, `wide hud and menus:`, `wide menus:`, `widescreen:` | `edit_name`: the widescreen layout edits, by which screens they change | `wide_hud.c` |
| `<service>:` | The IPC service's own name (`audren:u:`, `nfp:user:`, ...), with `NX2SARIA_TRACE=ipc` | `ipc.c` |
| `<module>:`, `<module> data` | A module's name (`main`, `subsdk0`, `sdk`): an unresolved strong import, a data permission change | `loader.c`, `memory.c` |

## The check

`python tests/log_prefixes_check.py` runs without game files or a build, with the standard library alone. It reads
every `.c`, `.cpp`, `.h` and `.inc` file under `runtime/` (not `third_party/` and not the generated `*_spv.h`), finds
the log calls and their prefixes as [code/tests.md](code/tests.md) says, and compares them with the table under "Prefixes" above:

- A prefix the code writes that has no row in either table is a problem: the check prints its file and line and exits
  with 1.
- A row of the main table that no call writes any more is a warning: the row can go.
- `--unseen` lists the calls whose prefix it cannot read; `--self-test` puts each kind of fault into copies of the
  sources and this page and checks that it is reported.

What it does not see: the lines listed just above, `rt_fatal`'s message after `FATAL: ` (the error itself, often a
function's name), and the pieces of an `rt_logf` line after its first (they continue a line another call started).
It reads code under `#if 0` and in every `#ifdef` branch alike.
