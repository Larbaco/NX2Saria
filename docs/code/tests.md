# tests

One section per test, for what [building.md](../building.md#checks) does not already say: `tests/run.py` first, then
the tests in the order of their file names.

A check that is a Python file and a
C file of the same name has one section, under the Python file: the Python file builds the C file with the header or
source it names and runs it; the C program prints each failed check with its line and exits 1 on any. Where such a
check has a `compiler()`, it takes `$CC`, else MSYS2 UCRT64's clang, else the clang on `PATH`, as
`tests/diff/common.py` picks it but without needing Unicorn. "No GPU, no game files" is said once per section where it
holds.

## `tests/run.py`

`EXTRA` lists the tests whose names do not end in `_check` or `_test`. `tests/maxwell_warp_emu.py` is not among them:
it is the emulator the tensor checks use, not a test.

Runs the checks under `tests/` in parallel and prints one summary ([building.md](../building.md#checks) has the
commands). What its header said beyond that:

Each test is a script that exits 0 when it passes. The runner finds every `tests/*_check.py` and `tests/*_test.py` and
the few others `EXTRA` lists, and runs each with the interpreter the runner runs with, from the repository's root. Run
it with the Python the tests need: `.venv` (Python 3.12, `tools/requirements.txt`) for most, `.venv-py314` for the
ones that need `compression.zstd` (`--needs py:compression.zstd` picks those).

A test says what it cannot run without in comment lines of its own file, each at the start of a line (`markers()`
reads them and returns (requirements, args template)):

```
# needs: windows
# needs: sdl3, py:unicorn
# args: --cc {cc}
```

`needs` takes one or more requirements, comma separated; `has()` gives (present, the reason to skip when it is not)
of one:

| Requirement | Present when |
|---|---|
| `windows`, `linux` | the operating system |
| `game` | the game's files are extracted under `build/extracted` (`tools/extract.py`) |
| `exe` | the game's executable is given to the runner with `--exe` |
| `xlate` | the shader translator is there, `build/xlate/nx2navi_xlate` (`tools/build_xlate.py`) |
| `gpu` | a Vulkan device: on Linux a `/dev/dri/renderD*` node or `/dev/nvidia0`; on Windows taken as present |
| `display` | a desktop to open a window on: on Linux `$DISPLAY` or `$WAYLAND_DISPLAY` (`xvfb-run` gives one) |
| `sdl3` | SDL3's development files: the C compiler finds `<SDL3/SDL.h>` and links `-lSDL3` |
| `vulkan` | Vulkan headers of 1.4 or later where the C compiler looks (`CPATH` counts; Ubuntu 24.04's are 1.3; `tools/vulkan_pin.py` fetches the build's into `toolchain/vulkan-headers/include`) |
| `vma` | `vk_mem_alloc.h` where the tests look: `$VMA_INCLUDE`, MSYS2 UCRT64's `include/vma`, `toolchain/vma` (`tools/build_game.py` `vma_include()` fetches it there) or `/usr/include` |
| `pkg:<name>` | a library pkg-config knows (`pkg:sdl3`) |
| `py:<module>` | a Python module the runner's interpreter imports (`py:unicorn`, `py:compression.zstd`) |
| `file:<path>` | a file or folder, relative to the repository's root |
| `clang` | a C compiler: `$CC`, else clang on `PATH` (on Windows MSYS2 UCRT64's, `C:\msys64\ucrt64\bin`) |
| `<program>` | anything else: a program on `PATH` (git, objdump), MSYS2 UCRT64's on Windows too |
| `linux:<req>`, `windows:<req>` | `<req>`, on that operating system only (`linux:sdl3`: the Linux build of the test links SDL3, the Windows one does not) |

A test whose requirement is missing is skipped, with the requirement as the reason. `--with REQ` takes a requirement
as present and `--without REQ` as missing, whatever the machine has. `builds()` says whether the C compiler builds a
program (and links it with flags).

`args` gives the test's command line. `{exe}` is the `--exe` path, `{xlate}` the translator, `{cc}` the C compiler
(`$CC`, else MSYS2 UCRT64's clang on Windows, else clang) and `{root}` the repository's root.

`tests/known_failures.txt` lists the tests that fail on main today, each with its reason, optionally only on some
operating systems, and the tests that fail only now and then (flaky):

```
name: reason
name [linux]: reason
name [flaky]: reason
name [flaky:case, flaky:other_case]: reason
```

A known failure still runs. It is counted apart and does not fail the run; one that passes is reported, so its line
can go (and fails the run with `--strict`). A flaky test counts as passed when it passes; when it fails it is reported
as flaky and does not fail the run. With cases named, only a failure of those unittest cases is flaky (the `FAIL:` and
`ERROR:` lines of the test's output, which `FAILED_CASE` matches); a failure of any other case is a failure.
`read_known()` returns {name: (reason, systems or None, flaky)}, flaky None for a known failure, () for a flaky test
and the case names for flaky cases; `#` comments and blank lines of the file are left out. `flaky_failure()` says
whether a failed run of a flaky test is the flakiness listed: any failure when no case is named, else one in which
every case that failed is named.

```
python tests/run.py [filters...] [--jobs N] [--timeout S] [--list] [--show-output] [--needs REQ]
                    [--with REQ] [--without REQ] [--exe PATH] [--strict]
```

A filter keeps the tests whose name holds it (`vk`, `cfg_rows`, `diff/`); with several, a test that holds any of them.
Each test's output goes to `build/test-runs/<name>.log`, and the last lines of each failure are printed after the
summary (all of it with `--show-output`). Exit code 0 when every test that ran passed or failed as known, 1 when one
failed or timed out, 2 for a bad command line.

`discover()` returns every test as (name, path): the name is the path under `tests/` without `.py`, with `/` between
folders. `Runner.run()` returns (state, seconds, log path), the state 'pass', 'fail' or 'timeout'; after a timeout it
kills what the test left running in its process group. In `main()` the results are name -> (status, seconds, log path
or skip reason). The `--help` description is the string `DESCRIPTION`.

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

## `tests/fatal_line_check.py`

Runs the game executable's busy crash probes (`runtime/main.c` `probe_busy()`, `probe_crash_stuck()`), each in a
temporary folder of its own. `RUNS` (20) times each, `--probe-crash-busy` and `--probe-fatal-busy` must end with 71 and
70 and exactly one FATAL line in the whole format the crash report (`CRASH_LINE`) or the probe's fatal error
(`FATAL_LINE`) writes, with no other thread's text inside it; the check counts how many runs failed. Then
`--probe-crash-stuck` must end with 71 within `TIMEOUT` (60 s), a whole FATAL line and one line that says stderr's lock
was given up. Needs the executable (`--exe`); no game files, GPU or window.

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

## `tests/ngx_lock_check.py`

Two checks of the lock every NGX call goes through (`runtime/vk/ngx_lock.h`). No GPU, no NGX, no game files.

- `runtime/vk/vk_dlaa.c` is read: every call to an NGX entry point (`N.init`, `N.req_ext`, `N.caps`, `N.alloc_params`,
  `N.destroy_params`, `N.create`, `N.evaluate`, `N.release`, `N.shutdown1`, `N.feat_inst_req`) and to a callback NGX
  hands out (`(NgxParam *))cb)(`) must have `NGX_CALL(` before it on its line. Each one that has not is printed with its
  line; any fails the check.
- `tests/ngx_lock_check.c` is built and run with the header over a fake NGX that counts how often a call enters while
  another thread is inside, and pthread mutexes for the platform's. Three threads as in the game: a replay thread (150
  calls of 0.2 ms, one of them a 30 ms create), a present thread (150 calls of 0.2 ms) and the frame's bookkeeping (15
  of 0.02 ms). Without the lock the fake must see overlaps (the detector works); with it none, every call counted for
  its caller, and the longest hold the replay's create. It prints the lock's log line, the present thread's worst wait
  and what an uncontended call costs (fails above 2 us). The worst wait is printed, not checked: the lock is not fair,
  so a waiter can sit through two holds in a row, and on a loaded CI runner the present thread waited 65.6 ms behind a
  32.8 ms hold.

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

## `tests/launcher_game_end_characterization_test.py`

Exercises the existing Tk `watch_game()` adapter with a synthetic session, a temporary game log and stubbed collaborators. The tests preserve
the order of window restoration, end logging, crash-summary synthesis, game-log annotation, staged-update check,
reports and the home screen. They cover a game that is still running, an absent session, disabled reports, an empty
note, a focus error and both update and reports short-circuits. Existing `launcher_test.py` cases exercise the actual
`GameRun` restart and fatal-end behavior with synthetic files and processes.

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
ending first, so its lines reach the file past the cap. `stuck`: without a log file, at the end, stderr's lock held
for good: every line still reaches stderr, after one line that says the lock was given up, and the lock is tried
`ERR_LOCK_WAIT_MS` times in all (`err_lock_final()`). No game files; it takes the path to clang as an optional
argument.

## `tests/log_line_check.py`

Builds and runs `tests/log_line_check.c` against `runtime/log_line.h`: pieces that make one line, an empty line, a line
that fills the buffer to its last byte, one byte more, a long line cut with ` [line cut]`, and the next line starting
clean after a cut.

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

## `tests/lowlat_revision_check.py`

Builds `tests/lowlat_revision_check.c`, which includes `runtime/pacing/vk_lowlat.c` whole over stubs, and runs it once
for a driver of revision 1, 2 and 3 of `VK_NV_low_latency2`: what the path may call on each ("The extension's
revisions" in [pacing.md](pacing.md)). It needs a C compiler, `vk_mem_alloc.h` and the Vulkan headers the runtime
builds with: no GPU, no game files.

The driver is `fake_dpa()` and the `drv_*()` functions. Its `vkGetLatencyTimingsNV` of revision 1,
`drv_timings_rev1()`, has that revision's four parameters and only counts the call and keeps its fourth argument,
which it does not follow; called with the three arguments of revision 2 it finds there whatever the register held, and
the check prints it. The one of revision 2 and later fills 16 reports that average to known times. `InfoRev1` is the
info structure as revision 1 had it; the static assertions hold `pTimings` at 0x10 in it and the count at 0x10 with
`pTimings` at 0x18 in the header's.

`main()` plays 4200 frames, so two statistics lines are due (frames 600 and 4200), and checks for every revision the
revision in the first line, one sleep mode set, a sleep for every frame start but the first (no host present yet) and
seven markers a frame: low latency itself runs the same on all three. Revision 1: no call of `vkGetLatencyTimingsNV`,
the first line says the timings are left out, and one statistics line of the two says there is no report. Revision 2
and 3: two calls and the report in both lines. `listed()` checks `vk_lowlat_revision()` on a made-up extension list.

## `tests/make_game_gate_test.py`

`tools/make_game.py`'s compiles through the memory gate (`tools/memgate.py`), with a stand-in script as the compiler
(it writes the object and its dependency file after a short wait, so that compiles overlap). No network, no game files.

- `compile_all` starts the compiles in the order of their cost (a lifted chunk's from the lift's manifest, another
  source's size) or of the seconds an earlier build measured, and the gate admits them in that order. In the case
  `big.c` is the largest file but the cheapest chunk and `rt.c` is not lifted, so its size is its cost; with no free
  memory the first compile starts (`min_jobs`) and the next is held for memory. A source that is not a lifted chunk is
  saved with cost 0, which keeps it out of the size model's scale.
- A learned time orders before the cost: with `b.o` 1 s, `c.o` 2 s and `d.o` 10 s measured last time, their median rate
  (`c.o`'s, 1 s per 1000 bytes of cost) puts `a.o`, not built before, at 4 s.
- A stop ends the wait of a compile held for memory at once; neither it nor the one behind it compiles.
- A peak measured while the free memory was below the 1 GiB reserve keeps a higher record (64 GiB planted, far above any
  real peak); one measured without pressure replaces it.
- Each successful compile's peak and time go into the estimates, which are saved; a compile the gate held shows in its
  summary; the lines the build prints about the gate (the closing lines with one peak at half its estimate and one at
  twice).
- `chunk_costs` reads `cost` and `switch_fn` from a module's `manifest.json`, `bytes` from a manifest older than `cost`,
  and a file's size when the manifest does not list it.
- `LifterEnvTest`: `recomp/gen.py` `lift_gate()` keeps the compiles' reserve and follows `--no-mem-gate`; without
  options the environment passes as it is.

## `tests/make_game_sdl3_test.py`

`tools/make_game.py` on Linux with or without the system's SDL3 (issue #11: Ubuntu 24.04 and Linux Mint 22 package
none). No network, no game files. `stopped()` gives (the value, '') of a call, or (None, the error text
`make_game.fail` printed).

- `sdl3_choice` takes the system's SDL3 when the compiler finds its headers, so a distribution that packages SDL3
  builds with the same commands as before (the link line of before, so nothing links again), else the build kit's
  (`tools/sdl3_pin.py`). `NX2SARIA_SDL3` = `kit` or `system` decides instead (the older spelling `NX2NAVI_SDL3` is read
  too), and a wrong value or a choice that cannot be met stops with the reason.
- The kit's SDL3 goes ahead of the system's folders for the runtime's sources only, the link names the kit's library by
  its path, and the check's program runs with the library beside it, as the game will. The link key names the kit by
  its place in the kit, not by the folder an AppImage is mounted in this time; with the setting unset `sdl3_tag()` is
  empty, the compile key of every game built so far.
- The kit's `libSDL3.so.0` goes beside the game, and goes away again when the game is linked against the system's (the
  runpath would load the kit's copy ahead of the system's); a copy with the kit's bytes is still the kit's after the
  build cache was deleted; a `libSDL3.so.0` this build did not put there stays, with a line that says so.
- `PreflightWithTheKitSdl3Test` runs `make_game.preflight` with every command recorded instead of run: the check's
  program is built and run in the build cache, not in the system's temporary folder; a program that does not start for
  want of a library the kit's SDL3 needs names it and the package that holds it; one ended by a signal leaves nothing
  on its stderr; headers without a library point to the kit's, and there is no program to run without its link.
- `Sdl3VersionTest` compiles `make_game.SDL3_PROBE` against headers that declare what it uses as `SDL3/SDL.h` does.
- `KitsCarrySdl3PinTest`: `make_game.py` imports `sdl3_pin` inside a `try`, so a kit without it would fail only on a
  Linux system without SDL3, asking for a package Ubuntu 24.04 does not have. The zip kit lists it, and the AppImage's
  kit copies all of `tools/` (`tools/make_appimage.py`) and carries the files besides.
- The build log's line names the compiler, its version and where SDL3 came from.

## `tests/maxwell_warp_emu.py`

A 32-lane Maxwell warp emulator for checking the SM86 tensor lowering on the CPU. It is not a test:
`tests/tensor_lowering_check.py` runs the full check of the tensor lowering on top of it (guards, untouched registers,
several initial fills) and `tests/maxwell_warp_emu_selftest.py` checks the emulator itself.

`tools/xlate/AmpereLowering.cs` rewrites SM86 instructions as Maxwell (64-bit) instructions, and the patched shader
translator turns those into SPIR-V. This file executes the Maxwell words the way that translator does: fields are
decoded exactly as its `Decoders/InstDecoders.cs`, opcodes are matched with the bit patterns of
`Decoders/InstTable.cs`, and each instruction does what its `Instructions/*.cs` emit for it, not what a real Maxwell
GPU does where the two differ. With it, the result of one lowered SM86 instruction (HMMA, LDSM, MOVM, HFMA2.RELU, ...)
can be compared with vectors captured on real hardware without running the game.

State per lane: R0-R254 (RZ = R255 reads 0, writes are dropped), P0-P6 (PT = P7 is true, writes dropped), the lane id,
local memory; per warp: shared memory. Every lane is active at the start; a guard (bits 16-19: predicate 16-18,
inverted 19) masks the writes of one instruction, EXIT retires lanes. Branches must be uniform across the active lanes.

Translator semantics it follows, and real hardware may not:

- Half-precision arithmetic (HADD2, HMUL2, HFMA2) works in F32 on the unpacked halves and rounds to F16 once at the end
  (`InstEmitFloatArithmetic` `EmitHadd2Hmul2` / `EmitHfma2`, `InstEmitHelper` `GetHalfPacked`: `PackHalf2x16`). HFMA2
  is therefore an F32 fused multiply-add rounded to F32, then to F16.
- That rounding is skipped when the packed value goes straight into another unpack: `Optimizer.PropagatePack` turns
  `UnpackHalf2x16(PackHalf2x16(a, b))` into a (or b). So F2F.F16.F32 then F2F.F32.F16 of the same register, or HFMA2
  feeding HFMA2, passes the F32 value on unrounded; an integer instruction (SHFL, LOP, ISCADD, ...) in between keeps
  the rounding, a MOV does not. Modelled per register (`Warp.halves()`); a guarded write and a branch end it.
- FTZ, FMZ and the rounding-mode fields of FFMA / FMUL / FADD / HFMA2 / HMUL2 are ignored, as the translator ignores
  them. Denormals are kept (IEEE); saturation is clamp(x, 0, 1) with NaN giving 0 (GLSL `FClamp` as
  min(max(x, 0), 1) on a GPU whose min/max return the non-NaN operand).
- PRMT is not implemented in the translator (1.3.2): `InstEmit.PrmtR/PrmtI/PrmtC/PrmtRc` only log "not implemented",
  so the destination keeps its old value. That is the default of the `prmt` argument; `prmt="maxwell"` gives the
  hardware byte permute instead, to see what the lowering intended.
- SHFL follows the translator's `HelperFunctionManager` `GenerateShuffle*` helpers (clamp and segment mask from operand
  C, the lane's own value where the source lane is out of range).
- LDS / LDL read the 32-bit word at (address >> 2) and extract a sub-word at (address & 3) * 8 (`InstEmitMemory`
  `EmitLoad`); STS of 8 and 16 bits stores those bytes at the address.
- Condition codes (.CC, .X) are not modelled: an instruction that writes or reads them raises.
- NaN results carry numpy's payload and sign. Neither is defined in SPIR-V, so result comparisons (`same_value()`) let
  a NaN match any NaN for float results.

Library use: `run(words, regs_in, smem=None)` takes Maxwell 64-bit words and returns a (32, 256) uint32 array (row =
lane); `run_warp(words, regs_in, smem=None)` returns the whole `Warp` (regs, preds, smem, warnings). Command line, for
vectors written by the tensor ground-truth capture:
`python tests/maxwell_warp_emu.py <vectors.json> [--xlate EXE] [--prmt ryujinx|maxwell] [--case NAME] [--trace] [--fill HEX] [--exact]`.
In `main()` one broken case does not stop the run.

`UnknownOpcode` is a Maxwell word the emulator has no decoder entry for; `Unsupported` a known instruction in a mode or
form it does not implement.

### Bit-level helpers

- `u32()`: a lane vector (or broadcast scalar) as uint32, wrapping Python ints to 32 bits. `sext()`: sign-extends a
  bits-wide field to a Python int.
- `half_lo()` is `UnpackHalf2x16Low`: the low 16 bits as F16, widened to F32 (exact); `half_hi()` the high ones.
  `pack_half()` is `PackHalf2x16`: each F32 rounded to F16 (round to nearest even, numpy's conversion), lo in bits
  0-15.
- `fma_f32()`: F32 fused multiply-add, rounded once (`FPFusedMultiplyAdd` -> GLSL.std.450 `Fma`, fused on the target
  GPUs). a * b is exact in F64 (2 x 24 significant bits). The F64 sum s may be inexact; TwoSum gives its error e
  exactly (s + e == a * b + c). Rounding s to F32 is then correct except when s lies exactly halfway between two F32
  values, where the sign of e decides the direction: the code takes the F32 neighbour of r on the side of s when e
  points towards it. F32 halfway points are F64 values, so no other case can double-round.
- `saturate()` is `FPSaturate`: clamp to [0, 1], NaN to 0 (fmax/fmin return the non-NaN operand). `abs_neg()` is
  `FPAbsNeg`: abs first, then negate (sign-bit operations in SPIR-V; numpy's abs/negative act on the sign too).

### `Ins`

One Maxwell word with the field accessors `InstDecoders.cs` uses, the fields shared by most decoders first.
`Ins.imm20()` is `InstDecoders` `Imm20`: bits 20-38, bit 19 from bit 56. `Ins.imm20_sint()` is `InstEmitHelper`
`Imm20ToSInt`; `Ins.imm20_float()` is `Imm20ToFloat`.

### `Warp`

Registers not given by the caller start as `fill` (SPIR-V locals are undefined unless the job zeroes them; a poison
value such as 0x7fc0dead shows reads of registers the lowering never wrote).

The translator's `Optimizer.PropagatePack` turns an `UnpackHalf2x16` of a `PackHalf2x16` result into the pack's F32
operand, so that half is never rounded to F16. Kept per lane and register: the F32 halves of the pack that wrote the
register (`sh_ok`), until the register is written otherwise. A write under a guard ends it in every lane: the
translator puts the instruction in an if-block and merges old and new value in a phi, which is no pack. `unconditional`
says the current instruction's guard is PT (`run_warp()` sets it).

Register file access: `R(n)` reads a lane vector, `W(n, v, mask)` writes the lanes in mask. `halves`: the F32 values a
`PackHalf2x16` rounded into v; `copy_from`: v is a copy of that register (MOV), pack and all. `Warp.halves()` gives
`UnpackHalf2x16Low` / `High` of register n as F32, after PropagatePack (the pack's operand where one wrote n).
`Warp.cbuf_word()` is c[slot][word_offset * 4] (`InstDecoders` `CbufOffset` counts words).

### Instruction handlers

Each handler takes (warp, ins, mask): mask = the active lanes whose guard holds. Lanes outside mask must not change.
What each follows in the translator:

| Handler | Follows |
|---|---|
| `op_mov_r()` | `InstEmitMove` `MovR`: SrcA in bits 20-27; a Copy, which PropagateCopy sees through |
| `op_mov_i()` | `MovI`: `GetSrcImm(op.Imm20)`, the raw 20-bit field (not shifted or sign-extended) |
| `_iadd()` | `InstEmitIntegerArithmetic` `EmitIadd`: AvgMode 1 negates B, 2 negates A |
| `_iadd3()` | `EmitIadd3` `Extend`: HalfSelect 1 = low 16 bits, 2 = high 16 bits |
| `_iscadd()` | `EmitIscadd`: (A << shift), then the AvgMode negations, then A + B |
| `_shl()` | `InstEmitShift` `EmitShl`: .M masks the count to 5 bits, else counts >= 32 give 0 |
| `_shr()` | `EmitShr`: .BREV (40) reverses A first, .S32 (48), .M (39) |
| `_lop_pred()` | `EmitLopPredWrite` |
| `_lop()` | `InstEmitIntegerLogical` `EmitLop` |
| `op_lop32i()` | `Lop32i`: no predicate output (DestPred PT) |
| `_lop3()` | the LOP3 truth table (`Lop3Expression.GetFromTruthTable`): bit (a << 2 \| b << 1 \| c) of lut, per bit |
| `_prmt()` | `InstEmit.PrmtR` / `PrmtI`: "not implemented", nothing is emitted, the destination keeps its value |
| `op_sel_r()` | `InstEmitMove` `EmitSel`: P ? A : B |
| `_isetp()` | `InstEmitIntegerComparison` `EmitIsetp` (not extended) |
| `_p2r()`, `op_p2r_i()` | `InstEmitPredicate` `EmitP2r`; `P2rI` takes the raw Imm20 as mask |
| `_r2p()`, `op_r2p_i()` | `InstEmitMove` `EmitR2p`; `R2pI` takes `Imm20ToSInt` |
| `op_s2r()` | `InstEmitMove` `S2r`: SR_LANEID; one warp, so TID.X is the lane and TID.Y/Z are 0 |
| `op_shfl()` | `InstEmitWarp` `Shfl` + `HelperFunctionManager` `GenerateShuffle*` (subgroup size 32); modes 0 IDX, 1 UP (signed compare, `ICompareGreaterOrEqual`), 2 DOWN, 3 BFLY. A predicated SHFL becomes an if-block in SPIR-V: reading a lane outside it is undefined |
| `_mem_load()` | `InstEmitMemory` `EmitLoad`: per word, the word at (address >> 2), sub-words extracted at (address & 3) * 8 |
| `_mem_store()` | `EmitStore`: 32-bit words at (address >> 2); shared 8/16-bit stores write those bytes at the address, local ones insert into the word at (address & 3) * 8. Lanes store in lane order (the last lane wins a conflict) |
| `op_f2f_r()` | `InstEmitConversion` `F2fR` / `EmitF2F`; the rounding field is `IntegerRound`: 4 round, 5 floor, 6 ceil, 7 trunc; an F16 result is `WriteFP`: `PackHalf2x16(x, 0)` |
| `op_mufu()` | `InstEmitMultifunction` `Mufu`: `FPAbsNeg` of Ra, the function in F32, `FPSaturate` |
| `_ffma()`, `op_ffma32i()` | `EmitFfma` (rounding mode and FMZ ignored); `Ffma32i`: C is the destination register |
| `_fmul()` | `EmitFmul`: A times the scale first, then A * B |
| `_fadd()` | `EmitFadd` |
| `_hadd_hmul()` | `EmitHadd2Hmul2`: F32 add/multiply per half, saturate, pack |
| `_hfma()` | `EmitHfma2`: F32 fused multiply-add per half, saturate, pack |
| `op_hmul2_r()` | `Hmul2R`: the NegA bit (31) negates B |
| `op_hfma2_r()` | `Hfma2R`: NegA (31) negates B, NegC (30) |
| `op_hfma232i()` | `Hfma232i`: C is the destination register, H1_H0 |

Half precision follows `InstEmitHelper` `GetHalfSrc` / `GetHalfUnpacked` / `GetHalfPacked`:

- `half_src()`: the two F32 values of a packed-half register operand: swizzle 0 (H1_H0), 1 (F32: the register as one
  F32 twice), 2 (H0_H0), 3 (H1_H1); then abs, neg. The halves come through `Warp.halves()` (PropagatePack).
- `half_imm_pair()`: `GetHalfSrc(immH0, immH1)`: each 10-bit field shifted left 6 as an F16.
- `half_pack()`: `GetHalfPacked`: 0 F16 pair, 1 F32 (lane 0's result), 2 MRG_H0 (high half kept), 3 MRG_H1 (low half
  kept). Returns the register value and the F32 halves `PackHalf2x16` rounded (None for the F32 format). The old halves
  are `GetHalfDest`: an `UnpackHalf2x16` of Rd.

Control flow is handled by the executor: EXIT retires lanes, BRA moves the warp.

### `decode()`

(name, handler) of a Maxwell word. The patterns are written as the translator's `Decoders/InstTable.cs` writes them
(bit 63 first, x = any). The most specific matching pattern wins, as `InstTable`'s XBits ordering does.

### `slot_address()`

Byte address of instruction k in a compute program as `AmpereLowering.Lower` lays it out (a scheduling word first in
every 32 bytes; `--lower` prints the instruction words only).

### `run_warp()`, `run()`

`run_warp()` executes the Maxwell words on one warp and returns its final state; `regs_in` in any form `load_regs()`
takes. Where control flow joins (branch targets) the translator merges registers in phis, and an `UnpackHalf2x16` of a
phi is not folded: the PropagatePack state (`Warp.sh_ok`) is dropped there and at every BRA (conservative: a register
no path writes needs no phi).

### Vector files and the `--lower` bridge

- `parse_int()`: an int, or a hex string (with or without 0x). `parse_reg()`: ("R", n) or ("P", n) for a register name:
  an int, "R12", "12", "RZ", "UR3" (needs urbase), "P0", "PT".
- `load_regs()` normalises register specs to {lane: {(kind, n): int}}. It accepts {lane: {reg: value}} with lane keys
  as ints or strings (and "*" / "all" for every lane, applied first so per-lane entries override it), or a list with
  one entry per lane; each lane entry a dict {reg: value} or a list of values indexed by register number; values ints
  or hex strings. `_normalised()` passes `load_regs()` output through unchanged (keys (kind, n)) and loads anything
  else.
- `lower()` runs `nx2navi_xlate --lower` on SM86 instructions (a list of [w0, w1, w2, w3] or one such list) and returns
  (Maxwell words, urbase, local bytes).
- `_fmt()`: a register value as hex plus its F32 and half-pair readings.

### Comparing results

A float result is right when its bits are, except that any NaN matches any NaN: SPIR-V leaves the payload and sign of a
NaN to the host (Vulkan spec, "Precision and Operation of SPIR-V Instructions"), and this emulator's numpy arithmetic
picks its own, while SM86 hardware writes 0x7fff for every F16 NaN result. Whether a value is NaN is the lowering's
doing; which NaN is not. Moves (LDSM, MOVM) and integer results compare bit for bit.

- `result_kind()`: how the destination registers of an SM86 instruction compare: "f16x2" (two F16 values), "f32" or
  "bits". `sm86` is [w0, w1, w2, w3] or a list of them (the last one decides). Opcode 0x23C is HMMA, with .F32 (bit 76)
  or .F16 results; (op & 0x1FF) in 0x030, 0x031, 0x032 is HADD2 / HFMA2 / HMUL2, with F32 results when bit 78 is set.
- `same_value()`: got matches want: equal bits, or for float kinds NaN where want is NaN.
- `ulps()`: distance in units in the last place (the larger of the two halves for "f16x2"); 0 for matching NaNs, a
  huge number for NaN against a number. +0 and -0 are 0 apart (`same_value()` still tells them apart).
- `run_case()` lowers, runs and compares one vector case and returns (ok, lines of report). Float results compare with
  `same_value()` unless exact. LDSM cases: "lane_addresses" hold Ra per lane when `regs_in` does not, relative to
  "smem", which sits at shared address "smem_base". Shared memory is padded to 64 KiB so stray reads stay in range.

## `tests/maxwell_warp_emu_selftest.py`

Self-test of `tests/maxwell_warp_emu.py`: hand-encoded Maxwell sequences against hand-computed results. The encoders
use the field positions of the translator's `Decoders/InstDecoders.cs`; `PT` is the guard field for predicate PT
(7 << 16), and `isetp_i()` encodes ISETP.cmp Pd, PT, Ra, imm, PT (AND).

`part1()` runs words encoded in the file:

- IADD32I, MOV32I, IADD3 with a negated operand, ISCADD; S2R lane id, LOP32I, SHL/SHR, LOP3, SHR by register.
- SHFL.IDX: the whole warp by a lane register ((lane + 1) % 32); a segment of 4 lanes (0x1C03: lane (t & ~3) | 1);
  segments of 8 with clamp 3 (0x1803), where index 5 is invalid and index 2 valid; BFLY 1; DOWN 2 with clamp 0 and
  with 0x1F; UP 1. Out of range lanes keep their own value.
- STS / LDS: 32, 16 and 8 bits, sign extension, 64 and 128 bits, the immediate offset (word [0x100 + 4t] = t, then
  R12-R15 = words 0..3 of the table).
- F2F: F32 from either F16 half (exact), F16 from F32 (round to nearest even, high half zero): 1 + 2^-11 is a tie and
  goes to even (1.0), 1 + 3 * 2^-11 to 1 + 2^-9, 70000.0 overflows to inf.
- HFMA2: per half F32 FMA then F16; neg C; saturate. (1 + 2^-10)^2 = 1 + 2^-9 + 2^-20 -> F16 1 + 2^-9; 1 + 2^-11 is an
  F16 tie -> 1.0.
- `Optimizer.PropagatePack`: an F16 result unpacked again without an integer operation in between is the unrounded F32
  value, directly or through a MOV; a LOP in between, or a guarded pack (P0 = lane < 32, true everywhere), keeps the
  F16 rounding. HFMA2 feeding HFMA2: 40000 * 2 is inf as F16, times 0.5 again.
- FFMA: rounded once. The exact a*b + c lies just below an F32 halfway point; F64 alone would round to the tie and then
  to even (0x3f800002), the fused result is 0x3f800001.
- PRMT: a no-op as the translator translates it, the Maxwell byte permute otherwise (0xbb99: sign of bytes 1 and 3).
- Predicates: ISETP (P0 = lane < 16), a guarded write, an inverted guard, EXIT of some lanes (lanes >= 16), a uniform
  BRA over one instruction (the skipped one would write 0xDEAD).
- Unknown opcodes raise with the word.

`part2()`, when `build/xlate/nx2navi_xlate.exe` exists, lowers SM86 instructions with `nx2navi_xlate --lower` and
checks the emulated result against exactly representable reference values (small integers, so every F16 rounding is
exact and only the fragment layout can be wrong). `tests/tensor_lowering_check.py` checks the same lowerings against
hardware results.

- `hmma_case()`: HMMA.{1688,16816}.F16 R4, R8, R12, R16 with small integers; returns (sm86, regs_in, expected D regs).
- LDSM.16.M88.4 R4, [R1] and LDSM.16.MT88.1 R8, [R1]: matrix m row r at 0x200 * m + 0x20 * r (16 bytes used), element
  (m, row, col) = 0x1000 * m + 0x100 * row + col. Transposed, a lane t holds column t/4 of rows 2(t%4) (low) and
  2(t%4)+1 (high); else row t/4, columns 2(t%4) and 2(t%4)+1.
- MOVM.16.MT88 R2, R1: element (row, col) = 8 * row + col at lane 4 row + col / 2, half col % 2.
- HFMA2.RELU R0, R1, R2, R3 (PT) as the translator runs it, whatever the scratch registers held before: the translator
  drops PRMT, so `tools/xlate/AmpereLowering.cs` `Half` builds the mask with integer operations. Negative sums become
  +0, positive ones stay (sums -2.0 and -0.75; 4.0 and -0.75).

## `tests/mem_budget_check.py`

Builds and runs `tests/mem_budget_check.c` on `runtime/mem_budget.h`, which turns the free share of the video memory
and of the system memory into a level (roomy, normal, tight, critical) with hysteresis, for the owners of memory that
can be given back. No GPU, no game files, any Python.

- The room itself: 8000 of 32000 free is 250 per mille; use past the limit and an unknown limit give no room;
  `mem_room_min` takes the tighter share and the smaller amount.
- Levels from a first sample, from roomy, as `runtime/mem_budget.c` starts (`room_pm()` is a share on a 32 GiB side):
  the lines at 40%, 20% and 8% belong to the level below; a large share of a tiny side (600 per mille, 200 MB) is
  critical, since less than `MEM_FLOOR_MB` free is critical at any share.
- Down at once, up only past the line by `MEM_HYST_PM`; a share that hovers around the 20% line (18.5% to 21.5%)
  changes the level once, not at every sample.

## `tests/mem_commit_launcher_test.py`

The launcher commit warning's pure threshold and message, with the system probe and the dialog stubbed. Yes is the
default button: `MessageBoxW` returns 6 (IDYES) and the flags carry neither `MB_DEFBUTTON2` nor `MB_DEFBUTTON3`
(`flags & 0x300` is 0).

## `tests/mem_commit_message_check.py`

Builds and runs `tests/mem_commit_message_check.c`, the pure Win32 commit-message selector; no Windows or game files
are needed. The advice is given for 1455 (`ERROR_COMMITMENT_LIMIT`), 8 (`ERROR_NOT_ENOUGH_MEMORY`) and 1450
(`ERROR_NO_SYSTEM_RESOURCES`), and not for 14 (`ERROR_OUTOFMEMORY`) or 487 (`ERROR_INVALID_ADDRESS`).

## `tests/mem_growth_check.py`

Builds and runs `tests/mem_growth_check.c` on `runtime/diag/mem_growth.h`, the memory report's growth bookkeeping:
which of the report's figures grew since the previous report and since play began, and which only ever climbed. No
GPU, no game files, any Python.

The checks feed the book reports as `runtime/vk/vk_memory.c` `growth_take()` does, once a period: a boot and a title
screen before the world clock runs, then play.

- `basics()`: the first report has nothing to compare with. Before play only the previous report counts, and only more
  than the step: +32, +64 and +64 are none past "more than 64", textures +65 is one. The base is the first figures in
  play, with no growth since play yet; in play both lists are given; a later report with no change keeps the second
  list and empties the first; when the world clock stops (title screen) it is still play.
- `noise()`: changes under `MG_NOISE` are neither rises nor falls (just under a MiB up; a MiB up from there; a MiB
  down); a fall of `MG_NOISE` ends "only climbs".
- `falls_and_cut()`: falls are ranked largest first after the growth; a cut buffer stays terminated and the count is
  still right (textures and other, twice).
- `session()`: the 15-minute session of 2026-11-13 at 1440p, one report a minute, with the figures of the log's
  `mem: by owner` and `mem: host` lines. Boot and title screen (the outside-VMA memory falls by 610 MiB when the boot's
  compute pipelines go), then play, where the textures and the device-local memory jump by gigabytes at the first area
  and then move with the areas, and the process's commit with them. The base is the first report in play, after the
  jump: the textures grew by 850 MiB since, and came down twice. 12 reports name a growth: the base and every report
  after it.

## `tests/mem_heap_check.py`

Builds and runs `tests/mem_heap_check.c` on `runtime/vk/vk_mem_heap.h`: when the memory report asks for the C runtime
heap's summary (`runtime/vk/vk_memory.c` `system_log()`), which holds every allocation of the game while it runs. Never
in play, where it held them for 54 to 70 ms at the first report of a session, unless `NX2SARIA_MEM_CENSUS`
(`mem_census`) asks for it. No GPU, no game files, any Python.

`tests/mem_heap_check.c` `session()` is a session as the report sees it: a report every 3600 presents, play from
`play_at` on; the summary takes `ms` milliseconds each time and counts as slow past 2 ms (`vk_memory.c`
`HEAP_SLOW_NS`), as `system_log()` notes it. It returns the summaries taken in play, and through `*before` those
before it.

- The sessions of 2026-11-14: play from the first seconds on, the first report at present 3600 in play, the summary 54
  and 70 ms. The rule before asked there once (the hitch); now it asks never.
- A long title screen: two reports before play, each fast there, then play: asked twice, both before play.
- A slow summary before play (a heap that is large already at the title screen): once, and no more.
- With the census switch every report asks, in play too, slow or not.

## `tests/mem_need_check.py`

Builds and runs `tests/mem_need_check.c` on `runtime/mem_need.h`: whether the machine has the commit the game needs
(`runtime/memory.c` `mem_start_note()`). Without the differential tests' packages (an `ImportError`) it makes the same
compiler choice without them.

- The machine the need was measured on, before the game starts: 41,515 of 73,154 MB in use, the process at 369 MB;
  32,008 MB for the game: enough.
- The same machine with the game in play, 65,840 of 73,154 MB in use and 24,000 MB of them the game's: what the game
  holds counts toward its need, so a check made late does not call a fitting game short.
- A 32 GB machine with an 8 GB page file and 12 GB in use by other programs: 28 GB free, enough; with 20 GB in use, 20
  GB free: short. A 16 GB machine with a 4 GB page file never fits.
- The boundary: exactly the need is enough, one MB less is short.
- Not asked (0), and a platform without a known limit (-1): never short. A process reading of -1 (unknown) adds nothing
  to the room.

With the game executable's path (`mem_need_check.py [PATH_TO_GAME_EXE]`), `probe()` also runs the executable's
`--probe-memneed` (it reads this machine's commit, prints the answer and starts nothing) three times: with
`mem_need_mb` = 0 (not checked), with 1 (any machine has it) and with a value no machine has, where the log's warning
line must appear. It returns 0 when each answers as it must. Without a commit limit (a Linux machine with none) nothing
is short: the forced case cannot be shown there.

## `tests/memgate_test.py`

`tools/memgate.py`: the admission rule, the in-process `Gate` over fake free memory, resident sizes and clock (`Fake`:
free memory, resident sizes by pid, a clock and a log the tests set), the `Estimates` file, `run()`'s exact peak on a
real small child, and the `wrap` command line end to end with two wrappers at once. No game files; stdlib only; the
children allocate at most about 50 MB each (`ALLOC`: 50 MB, written, so resident).

- `AdmitTest`, the rule both gates share: owed is need less what is resident (2 GiB needed, 0.5 GiB resident: 1.5 GiB
  still owed); a compile without a pid yet owes all its need.
- `GateTest` (`GateTest.background()` acquires in a thread and appends the lease to `done` once admitted). Owed memory
  holds until the compile is resident: with 1 GiB of a 2 GiB need resident and already counted in the free memory, 3
  GiB free less 1 GiB still owed covers 2 GiB. Reserve and ceiling: a 5 GiB compile the memory holds is still over the
  limit by the 1 MiB, and runs alone by `min_jobs`. Unknown free memory admits what the RAM holds (three compiles of 3
  GiB: 9 GiB promised). Waiters are served in order: a small one that would fit does not pass a big one, and the order
  is the one the gate gave out under its own lock. A compile held for memory leaves the gate at once when its `cancel`
  turns true: `acquire` returns None (also when cancelled before it is even its turn), the waiters behind it move up,
  and nothing is left to release. A lease knows the least free memory while it ran (the first admitted by `min_jobs`,
  the second on 8 GiB free, then 1 GiB, below the 2 GiB reserve, while both run).
- `EstimatesTest`. After a re-chunk a chunk of the same name and size can hold another switch: its `switch_fn` must be
  within 20 % too, unless both are below `SWITCH_SMALL`. A peak measured while the memory was short (`keep_higher`)
  does not lower the record; without pressure the new peak is taken. The model is checked against the peaks it was
  drawn from (clang 20, -O1), within 15 %. A source with no cost is not part of the learned scale, and a scale far off
  is clamped. An entry from before `switch_fn` reads it as 0; garbled files are empty.
- `RunTest`: with SIGCHLD ignored `wait4` finds no child to wait for; the exit code comes from `Popen` and the peak is
  unknown.
- `AvailableTest`: on Windows the free memory is no more than what can still be committed (a small page file runs out
  first).
- `WrapTest`, wrappers at once over one state folder, as Ninja runs them (`WrapTest.finish()` gives (exit code, stdout,
  stderr) of each; a wrapper still running after the timeout fails the test; the child script can wait for a file
  another process makes). Gate off: each compile waits until the other has started, so both end well only if they ran
  at the same time. A small compile arriving after a held big one waits behind it, though the memory would hold it
  now; the big one, once running, owes all its need, so the third waits for it to end. Ended and reused entries are
  dropped (the test plants its own pid, as a wrapper that started at another time would leave it).
- `CgroupLimitTest`: `total_ram()` under a cgroup v2 limit is the tightest `memory.max` on the way up, so the reserve
  and the rule for unknown free memory go by what the build may use; under 6 GiB the reserve is 1.5 GiB, since 10 % of
  6 GiB is less than the minimum.

## `tests/menu_mouse_check.c`

The arithmetic of the mouse in Ocarina of Time 1.1.1's menus (`runtime/ui/menu_mouse_geom.h`); `tests/menu_mouse_check.py`
builds and runs it. No game files.

- A button's box from its pane's fields as the game's focus frame takes them. The flags word holds the base position h
  (0 left, 1 centre, 2 right) and v (0 top, 1 centre, 2 bottom). A centred pane 200 x 100 at (300, -50), scale 1; a
  left-top base, where the translation is the left edge and the top edge (y up: the box hangs below it); a
  right-bottom base with scale 2 and a mirrored scale (the size counts, not the sign); the extra size
  (main+0x769930): bit 10 with bit 9 clear adds to the height, with bit 9 set to the width; no area, or not a number:
  no box.
- The layout units across the picture from the projection: 16:9 gives 3840 x 2160; 21:9 (64:27) is squeezed by 0.75,
  so ±2560 = 1920 + u with u = 640, where `NX2SARIA_WIDE_MENUS` = `full` puts the menus' edge panes; 32:9 gives ±3840;
  a 1280 x 720 layout ±640 x ±360; a matrix that is no projection gives the 16:9 values and 0.
- The pointer in layout units and back. On 16:9 the picture's corners are the layout's; on 21:9 the layout's 3840 sit
  in the middle 16:9 (from 1/8 to 7/8 of the width).
- The box under a point is the smallest of those it is in (a slot inside its panel), -1 off them all; edges count as
  in. A `wide_menus` = `full` button moved out to x 2250 on a 21:9 picture is reached by the pointer at 95% of the
  width.
- Movement: a pixel of a 4K picture is movement, less is not.
- Clicks: a press counts once; a press given while no menu takes the mouse (synced there) is no click of the next
  menu: between menus a quick second click lands on the fade.
- The queue: A and B wait one at most, the wheel three; a turn the other way drops the rest; a drop empties it (four
  quick right clicks on the pause menu queue one B; the queue is dropped when the menu goes because a B closed it). L
  and R wait one at most, like A and B, and go with their menu.
- The nearest box when the pointer is in none: the bombchu slot's narrow box (-1196..-1096, -413..-193 as the 16:9
  pause menu has it) beside the bomb slot's, and a click on the middle of the bombchu's picture (-1225, -300).
- The pause menu's tab row: the four pages' names and the L and R signs, off the row nothing; a tab's place in the
  window and back, on 16:9 (the notebook) and on 21:9 with the menus over the whole picture; the page from its
  layout's name, and the step toward a clicked tab. A walk to a clicked tab stops when the page goes the other way
  (the player's own L or R), not on its own steps.

## `tests/mesh_fb_hold_check.py`

Builds and runs `tests/mesh_fb_hold_check.c` on `runtime/gfx/mesh_fb_hold.h`, the table behind the mesh fallback's
depth hold (`runtime/vk/vk_exec_draw.c`): a list whose colour draw was skipped for its pipeline holds its depth-only
draw for a few presents, and no longer. No GPU, no game files, any Python. The two lists are record addresses as the
game's memory has them (0x59220f600 and 0x26d876000).

- A frame's order is depth first, colour after: the stamp of one frame holds the depth draw of the next.
- While the colour draw keeps waiting, each frame stamps again and the hold goes on; when it is drawn the stamp goes,
  and the depth draw comes back in the next frame.
- A colour draw that no longer comes (the list left the view): the hold lapses by itself.
- A draw of one list does not clear another list's stamp, in whatever slot each lies.
- The present count's 24 bits wrap (77 hours at 60 fps): the hold counts across the wrap. The highest address the
  game's memory has keeps its bits above the count.
- Every index is inside the table, and a field of records (256 records of 12 bytes) spreads over it.

## `tests/mesh_invariant_check.py`

Mesh positions made Invariant (`runtime/vk/vk_pipeline.c` `spirv_mesh_invariant()`): builds the function, cut from
`vk_pipeline.c` by its text, and runs it on modules made in the test. No game files. Its `--help` text is the string
`DESCRIPTION`.

- A mesh shader whose position is a member of the per-vertex block (glslang's `gl_MeshVerticesEXT`); one whose position
  is an array variable of its own, as `tools/xlate/MeshSpirv.cs` declares it (an Output variable of vec4[max vertices]
  decorated BuiltIn Position), assembled; the same block mesh shader with the position already invariant; a vertex
  shader.
- The two mesh shaders get one Invariant decoration on the position (`OpMemberDecorate` on the block member,
  `OpDecorate` on the variable), the rewritten modules pass spirv-val, a second pass changes nothing, and the
  already-invariant mesh shader and the vertex shader are left alone.
- `invariants()` gives (`OpDecorate` Invariant count, `OpMemberDecorate` Invariant count) of a module; the table in
  `main()` maps a name to (changed, the Invariant decorations wanted after the rewrite as that pair).

## `tests/mesh_path_check.py`

Builds and runs `tests/mesh_path_check.c` on `runtime/vk/vk_mesh_path.h`: how the game's task and mesh programs are
drawn (`runtime/vk/vk_device.c` `create_device()`): natively, through the mesh fallback, the mesh emulation or not at
all, for each device kind and setting. No GPU, no game files, any Python.

- The default (auto, emulation off) by device.
- Intel's cards with mesh shaders take the fallback on every Intel driver: holes in the fog on Arc B390 and B580 with
  the native path (issue #13), which 0.18.2's 32-lane task stage and invariant positions did not mend.
- `mesh_fallback` = 0 gives an RX 6000 card native mesh pipelines back, as 0.10.1 drew them; `mesh_fallback` = 1 on
  every device.
- The mesh emulation acts only without mesh shaders, and then before the fallback. An NVIDIA card never takes the
  emulation, which ended the game inside its driver (GTX 10 cards in the low-end GPU mode of 0.10.1, which set
  `mesh_emulation` = 1).
- A value the settings table does not list reads as auto. The reason names the driver default, and how to turn it off.
- `mesh_fallback_batch` = auto batches where the fallback is the device's default (where `mesh_fallback` = auto would
  choose it).
- RADV inside the releases whose task shaders lose the device takes the fallback by default, and `mesh_fallback` = 0
  still draws natively there. The range is checked release by release: the last one before it, the first one in it,
  each side of a series' fixed release (with a table of its own, since no series has one yet), a series with no fixed
  release up to its last patch number, the snapshots a packager numbers 26.99.99 and the main branch's 27.0.0, and
  another driver with the same number. A card without mesh shaders is drawn for as before, and its reason does not
  name the regression.

## `tests/mesh_pipeline_check.c`

Injects cache-key results into the reporting guard used by `mesh_prepared_check` (`runtime/vk/vk_exec_draw.c`).

## `tests/meshfb_batch_check.py`

Runs the game executable's `--probe-meshfb` twice, with the mesh fallback's batch off and on
(`NX2SARIA_MESH_FALLBACK_BATCH`, `runtime/vk/vk_exec_draw.c` `mfb_draw()`, `runtime/gfx/mfb_batch.h`), validation on
in both runs. The probe (`runtime/gfx/mesh_fb_probe.c`) draws mesh programs of the runtime's own through the replay,
with and without a task stage, with direct and GPU-written counts, one and two in one pass; it compares each picture
of the fallback with the mesh pipeline's (where the device has mesh shaders), and checks that a draw past the mesh
buffer and a task draw past the task buffer are drawn whole up to what fits and are counted. With the batch on, a
task program's first draws (no history yet) go one by one in a pass whose other draws are batched, and the passes are
cut; the probe's `batch:` line must show draws batched, some one by one for want of history, passes cut, and no draw
without a pre-pass command buffer.

`run()` reads the probe's output. The draws' checks: none failed, the last check of the draw part ran, and with
batching on the batch line came. Validation errors while drawing count; those of the device's creation (extensions
newer than the layer) and of its destruction (objects the probe's teardown leaves, such as the pre-pass command pool)
are the probe's, not the batch's, and are only printed. So is how the process ended after the checks. The usage text
the test prints is its `USAGE` string.

## `tests/mfb_batch_check.py`

Builds and runs `tests/mfb_batch_check.c` on `runtime/gfx/mfb_batch.h`, the rule of the mesh fallback's batch
(`runtime/vk/vk_exec_draw.c` `mfb_draw()`, `NX2SARIA_MESH_FALLBACK_BATCH`): which draws are batched and which go one
by one (the way back), the parts of the buffers they take, and a program's history. No GPU, no game files, any Python.

`tests/mfb_batch_check.c` `sim_draw_ex()` is the draw path's order as `mfb_draw()` has it, without Vulkan:
`mfb_batch_need`, then `mfb_batch_steps` (the same function `mfb_draw()` calls) with the cut, the pre-pass command
buffer and the part as calls on the simulation, and `mfb_count`. A draw one by one after parts were taken closes them
(`runtime/vk/vk_mesh_fb.c`, `closed`; `vk_meshfb_one_by_one`): the next batched draw cuts. A draw one by one ends its
pass (`vk_exec_barrier`). The end of a pre-pass command buffer (a cut, or `pre_end`) starts the parts over. `FS` (100)
is the parts' floor in these checks (`mfb_floor`).

- The reasons, in their order.
- The parts' sizes. The floor is 1/`MFB_FLOOR_DIV` of the buffer, in the program's workgroups, never under the least.
  The case seen in Kokiri Forest: a program that had asked for 32 at most asked for 454.
- The cut: at a pass's first batched draw, and again whenever the pass it served ended.
- The parts: in turn, aligned, never past the buffers, and nothing taken when one does not fit.
- The history grows from the records read back. A history is taken once it has settled: `MFB_MIN_CALM` records in a
  row without a jump (more than twice the most). A batched draw short of its part excludes the program, which is taken
  back after `MFB_READMIT` calm records, until `MFB_MAX_SHORTS`. The draws already in flight come back short together:
  one exclusion counts once.
- A pass as `mfb_draw()` sees it: the way back to one by one, its reasons and the cuts. Closed with the pass still open
  (a draw one by one that did not end it): the next batched draw cuts too. A stage that reads a texture or image goes
  one by one, without a cut. When the cut fails the draw goes one by one (no pre-pass), and the draws after it are
  tried again. When the buffers fill, the draw that does not fit cuts and starts the parts over; a part larger than
  the buffers goes one by one. A batched draw whose stages failed to record is not batched, and its cut was for
  nothing.
- The task overflow (`runtime/shaders/meshfb_clamp.comp` keeps the count asked for): a batched draw short of its task
  part excludes.

## `tests/mods_romfs_check.py`

The mods folder served as RomFS overlays (`runtime/hle/romfs.c`, `runtime/patches/mods.c`): load order, added files,
case, Unicode. Run as `python tests/mods_romfs_check.py --exe build/v121-<tag>/nx2saria.exe --version v121`; it needs
the extracted RomFS of the version. The `--help` description is the string `DESCRIPTION`.

It builds a mods folder in a temporary directory with files of its own bytes at real RomFS paths (from
`build/extracted/<version>/romfs/index.json`), runs `nx2saria.exe --probe-fs` on it and reads the `mods:` lines of the
log. `--probe-fs` compares the bytes it reads through the RomFS with every overlay file, so a wrong winner fails it
too. `run()` gives the probe a profile of its own beside `mods`, since the merged resource size table is written
there. In `main()` one planted file is of another game version's (no RomFS has a `Product.999` table: counted, never
served) and one sits in a mod manager's `romfslite` folder.

`restbl_case()` uses a second folder for the merged resource size table the start writes to the profile
(`runtime/hle/romfs.c` `restbl_overlay()`): a grown file's entry, an added file's, a shipped table's and another
version's table's (a file smaller than the original: only its table raises it). The table's layout (RESTBL): "RESTBL",
u32 version, u32 name length, u32 hashed count, u32 named count, {u32 crc32, u32 size} sorted, then {char[name length],
u32 size}. `restbl_pack()` returns a table with the sizes of its hashed entries set from `entries` (crc32 -> size,
existing hashes only).

## `tests/motion_fusion_check.py`

Builds and runs `tests/motion_fusion_check.c`: `runtime/input/motion_fusion.c` on made-up sensor samples, 4 ms apart,
with a stand-in for the log (printed, and the lines kept for the checks) and no settings from the platform layer. The
program prints a PASS or FAIL line per case. `static_checks()` also checks that `motion_fusion.c` makes no log call
between taking the instance's lock and the last unlock in the functions that log. No game files, no GPU.

Build notes: `runtime/platform/linux.c`'s clipboard calls SDL3; the check never uses the clipboard, so a stand-in keeps
it from needing SDL3's headers and library. The platform files are the runtime's, built as the runtime builds them;
the strict warnings are for `motion_fusion.c` and the driver. `runtime/simd_fp.h` `FOK` ors two compares on purpose,
and clang 18 warns on it.

The simulated controller (`Sim`) has a true orientation T (controller -> world, z up), rotated by the true rate; its
sensors add `bias` (rad/s), `gyro_noise` (rad/s RMS per axis) and `acc_noise` (G RMS per axis). `sim_pitch()` sets the
true pose turned about the controller's x by a number of degrees from flat; `sim_run()` gives n samples turning at the
true rate w (rad/s, controller axes), the accelerometer reading gravity in controller axes. The error measured is the
angle between the reported up (the reference z in controller axes) and the true one, in degrees. `RAW` is no
calibration and no dead zone: the sensor's own rate.

1. Lying still with a bias and a little noise (5 s): learned within 1.5 s of being still, then the reported rate is
   none and the angle stops, with one log line per still period.
2. No calibration: the bias goes through without a dead zone; each dead zone level zeroes a rate just under it and
   passes one just over it unchanged.
3. Known rotations from lying flat: a quarter turn about up, a quarter pitch and back.
4. Held in the hand (35 s, 2 deg/s RMS of gyro noise in all, 0.01 G): tremor and a slow pan are never still, and
   nothing is learned.
5. Re-learning: still with a bias, held, then still with a bias 0.3 deg/s off it. In 3 s the controller is still from
   about 0.8 s, so the period learns for the 1 s that logs it; over 6 s, past 2 s learned in all, the bias follows with
   a 3 s time constant.
6. The bias of each controller comes back with it; five more sources push the oldest out of the four kept.
7. The turn and tilt factors scale the rate, the angle and the direction. With 1 and 1 the reported rate is the input's
   and the direction the filter's, bit for bit; back to 1/1 snaps to it.
8. The integral term: a constant bias about a horizontal axis that is never learned (calibration off), tilted 30 deg,
   over 60 s, against the proportional term alone.
9. The aggregate line after 60 s.
10. The quarter turn and restart that `runtime/hle/ov_probe.c` `probe_feedback()` checks.

Further cases: (a) larger biases lying still are learned too (the step rule waits until the bias has been learned for 2
s); (b) held pitched for 120 s with a bias about the controller's z, long enough for the integral to take part of it
up, then set down flat in 2 s and left still for 10 s and 50 s more: once the bias is learned, the integral no longer
turns the heading of the still controller (`heading_deg()` is the x axis' heading); (c) turn and tilt factors over
closed and non-commuting paths (`loops()`: five loops of 30 deg turns about the controller's x and y; a 45 deg/s turn
about the world's up): the reported orientation stays tied to the filter's; (d) a known limit: a smooth slow turn with
little noise looks like a still controller with a bias; (e) samples with values that are not finite move nothing and
poison nothing; (f) a wrong bias (a smooth pan of 3.5 s learned as one) is learned again once the controller lies
truly still (60 s, no bias).

## `tests/mouse_direct_check.py`

Checks the direct camera hook's register contract and raw-count routing without game data
(`python tests/mouse_direct_check.py --cc <clang>`; the `--help` text is the string `DESCRIPTION`). One block of
stand-ins for the rest of the runtime serves both harnesses. The sites are Ocarina 1.1.1's
(`recomp/patches-v111.txt`, set `mouse_direct`): the two sites of `camera_input_oot` and `camera_activity_oot`.

## `tests/mouse_stick_check.c`

The mouse camera in stick mode (`runtime/input/input_map.c` `input_mouse_stick()`): raw counts to a right-stick
deflection with the camera speed taken out of it. The game multiplies the right stick's turn rates by the camera
speed, so the deflection is divided by it, and the division comes before the limit to full tilt: a slow movement turns
as far as at camera speed 1, and a fast flick reaches full tilt, which the game turns into its top speed with the
factor in it. `tests/mouse_stick_check.py` builds and runs it with `runtime/input/input_map.c`; the stand-ins are what
`input_map.c` needs from the rest of the runtime, none of it reached from `input_mouse_stick()`. No game files.

- A slow movement: the deflection is the one at camera speed 1 divided by the factor (while that is below full tilt),
  so the game's multiplication gives the same turn. Full tilt is at 3000 counts a second.
- A fast flick reaches full tilt at every camera speed, in each direction. The case that was wrong: at camera speed 1.5
  a flick stopped at two thirds of full tilt, the top speed of camera speed 1.
- Nothing to divide by: no deflection.

## `tests/movie_timeline_check.py`

Compares both movie timestamp patches with Unicorn and their lifted C. It needs Unicorn and a C compiler (`--cc` or
`CC`), no game files and no GPU; the generated harness files stay in `build/test/movie_timeline`. The `--help` text is
the string `DESCRIPTION`. `main()` builds one synthetic layer -> controller -> player -> video surface chain for both
engines. The samples are the selected 60 Hz video's millisecond-rounded PTS at different redraw rates: pauses,
repeated pictures and skipped pictures must follow PTS, never the redraw count; the last ones hold a picture and then
start a new movie.

## `tests/nvidia_cache_check.py`

Builds and runs `tests/nvidia_cache_check.c` on `runtime/vk/nvidia_cache.h`: what the game puts into the environment
for NVIDIA's shader disk cache (a folder beside the exe, a limit of 8 GiB and, on Windows, the cleanup off; the
player's own values stand), how it counts the folder and the lines the logs say about both. It needs a C compiler
only: no driver, no GPU and no game files.

`wiring()` finds in the sources what the runtime does with the header (`runtime/vk/vk_device.c`
`nvidia_cache_beside_exe()`, `runtime/vk/vk_pipeline.c` `lw_say_cache_folder()`, the setting's rows in
`runtime/cfg/cfg.c`), so a change of that wiring shows in this check even where the Windows-only code does not run; it
returns 0 when the runtime applies the header where it says. Its table holds (file, what it must hold, a pattern);
`squeeze()` reads adjacent string literals as one.

In `tests/nvidia_cache_check.c` the lister answers from a planted tree: `empties` are folders that are there and hold
nothing, `denied` folders the system refuses (`fake_list()` answers 5, `PLAT_FS_ERROR` of `runtime/platform/platform.h`),
`listed` counts the lister's calls, and a folder some entry sits in below it exists too. `deaf_list()` goes on after
the callback says stop; `deep_deaf_list()` gives a folder and a file in each, whatever the callback says; `deep_list()`
one folder inside each one, for ever. `measure()` plants a sentinel past `cap`: the walk never writes past it.

- The plan: scenarios written out by hand (`in_of(windows, enabled, driver, path_set, size_set, skip_set, folder_ok)`),
  then what holds for every combination of the seven inputs: where the folder's state is not asked (`nvc_need_folder`)
  it changes nothing, and Linux never differs from what it did before: the folder, then the limit.
- The variables a restart inherits: the game names what it set, and a start takes those for the game's. `nvc_stale`:
  what the game set before and does not set now is taken out of the environment.
- The lines. Linux: the words the log has always had, built in the check with the old format. Windows: the three
  variables, with their values and whose they are. The lines when nothing is set, and when the environment refuses a
  variable. A buffer too short takes what fits, ends with a NUL and is not passed.
- `nvc_commas` (1234567 into a short buffer is cut, never overrun) and `nvc_size_text` (1739461181 bytes is 1.62 GiB).
- `nvc_measure`. Windows paths join with a backslash, the root's own kind. The path buffer: a folder whose path does
  not fit is said, the rest counted ("/a/bb" needs 6 bytes with its NUL; with 2 the root alone does not fit; with 3
  the root fits to the last byte and "/a/bb" does not). The entry limit; a lister that goes on after the callback said
  stop is not counted past the limit. The depth limit: a folder inside every folder, for ever, and nothing is counted
  after it, whatever the lister does with the callback's answer.

## `tests/ocarina_log_check.py`

Builds `runtime/pacing/frame_rate.c`'s ocarina log (`ocarina_log_init`, `ocarina_watch`, `ocarina_end`, cut out of the
file) with stand-ins for the patch sets, the patch page and the log, and plays it the page words the sets `orecord`
(+264) and `ostaff` (+776) write. A stretch of ocarina use gives one line after two quiet seconds, with its presents,
the presents the recorder clock moved on and the notes the staff moved (rises of its signal), and nothing comes while
neither word changes. Without the sets the log watches nothing and says so at the start. No GPU, no game files, any
Python. `cut()` takes `frame_rate.c` from the ocarina log's first `#define` to the end of `ocarina_watch`;
`compiler()` here takes `$CC`, else MSYS2 UCRT64's clang, else the clang on `PATH`.

## `tests/ocarina_timing_check.py`

Checks the set `orecord` of `recomp/patches-v111.txt`: the ocarina recorder's clock carries each update's fraction of
a 60 Hz frame on the patch page's +264 (`FRACTION`) instead of cutting it. It needs Unicorn, numpy and a C compiler
(`--cc` or `CC`), no game files; harness files stay in `build/test/ocarina_timing`. The `--help` text is the string
`DESCRIPTION`. `SIZE` (0x4ea0000) is a stand-in main size: the patch page lands at main+0x4ea0000, as on 1.1.1.
`GAME` (0x1e390008) is the game's word, `fcvtzu w8, s0`.

1. `check_table()`, the table: the set's switch and default, its four sites, their original word and the nine
   replacement words, the page word resolved to `adrp x8` of the patch page at each site, and no other set's word
   loading or storing the fraction's bytes through an unsigned offset. The set is found by its name: sets added after
   it go after it in the table.
2. `check_lifted()`, the lifted words (`recomp/lifter.py`, as the generator lifts them) against the game's word and a
   float32 model. `fcvtzu()` is `fcvtzu w, s`: toward zero, NaN and negatives to 0, saturated at 2^32 - 1; `model()`
   is the words in float32 with round to nearest: (w8, the new fraction's bits). Whole steps of one and two frames
   give the game's count, and the fraction stays +0.0. The steps of the 40 to 240 fps limits count 3600 frames a
   minute, each update the floor or the ceiling of the step; at 144 Hz it is 5 in every 12 updates, and the float step
   is a little under 5/12, so the first 12 count 4. A carried fraction: 0.75 + 0.5 gives one frame and leaves 0.25.
   Steps the game never gives (NaN, negative, infinite, huge): the game's count, a fraction in [0, 1), and the next
   whole step counts one. v0 keeps the step, its upper lanes too.
3. `check_unicorn()`: the nine words, the `adrp` included, run from the site's address at each site (FP and SIMD on:
   `CPACR_EL1` = 0x300000) against the lifted C, where x8 is set to the page buffer as the `adrp` sets it to the page,
   over random steps and fractions: registers and the page agree bit for bit.

`build()`: each function sets the host's floating-point mode from `c->fpcr` as the runtime does on entry to guest code
(`runtime/cpu.h` `rt_fpcr_sync()`) and puts Python's back after the words; x8 is the page, set by the caller as the
`adrp` would. `Harness` is a Cpu and a page buffer for the lifted words; `Harness.run()` makes one call and returns
w8. The Cpu changes only in x8, v16-v18 and FPSR, and the page only at +264.

## `tests/oq_hold_check.py`

Builds and runs `tests/oq_hold_check.c` on `runtime/vk/vk_oq_hold.h`: the count a samples-passed report writes when
the full occlusion query ring left some of its draws uncounted (`runtime/vk/vk_exec_xfer.c` `oq_report_count()`,
`NX2SARIA_OQ_HOLD`): the larger of what was counted and the record's last complete count. No GPU, no game files, any
Python.

- Nothing remembered: the old rule, what was counted and at least 1.
- A record reported complete, then a frame that missed draws: the last count holds. What was counted is a lower bound:
  above the held count it wins.
- A newer complete report replaces the held count, an occluded one too.
- Other records keep their own counts; a record next to it is another one. A record that shares a slot with another
  forgets the older count, never takes the other's.
- A report with no CPU-visible record (address 0) is neither remembered nor held.
- The 1024 records of a frame's queries spread over the table: few collisions among consecutive records.

## `tests/pack_shader_cache_test.py`

`tools/pack_shader_cache.py` on a fake shader cache: the pack layout `runtime/gfx/gfx_shader.c` reads (sorted index,
bounds, data), loose files folded in and deleted, a second run merging new loose files, a loose file winning over its
packed copy, malformed files skipped and `--keep`. No game files; stdlib only.

`USE` is a use record as `gfx_shader.c` `use_store()` writes it; `read_pack()` returns {(kind, name hex): bytes},
checking what `gfx_shader.c` `pack_open()` checks. `PackTest.shader()` makes `in/` again, as `in_store` does after a
pack removed it: `in/` is removed when empty. A bad loose copy leaves the packed one in place; a bad file that is
nowhere else is skipped, and never deleted unless packed.

## `tests/part_file_check.py`

Builds `tests/part_file_check.c` with the platform layer and runs it: `runtime/part_file.h`, which the runtime's
detached threads write their files through (the flight recorder's dump). A file written piece by piece gets its name
only when it is complete, so a game that closes under the writing thread leaves the older file or none, never one cut
short. The C side checks the helper itself; the Python side then kills a process in the middle of a write, as the
window's close ends the dump thread, and looks at what it left: the older file whole under its name, the half in the
`.part` file. The build adds `stand_ins()` of `tests/platform_stand_ins.py` for what the platform layer calls outside
it. No game files, no GPU. `part_file_check <folder>` runs the checks in that folder;
`part_file_check <folder> hang` writes half of `<folder>/killed.tsv`, prints "writing" and sleeps: the Python side
kills it there, as a closed game ends the thread that writes a dump. `rt_log()` in the C file takes the platform
layer's lines.

- A new file: nothing under its name while it is written, the whole file after.
- Over an older file: the old one stays whole until the new one is complete.
- Given up (the writer met an error): the older file stays, the part goes.
- A folder that is not there: nothing opens, and closing nothing is no success. A name that does not fit the path
  buffers.

## `tests/pass_hooks_check.py`

Builds and runs `tests/pass_hooks_check.c` on `runtime/vk/vk_pass_hooks.h`: which pass begin runs the frame's pass
hooks (`runtime/vk/vk_exec.c` `vk_exec_pass_begin()`). A parallel group's hooks run once, at its first chunk's first
pass: a pass that chunk begins again later must not put its own target back as the upscaler's light target after the
hooks of later passes ran. No GPU, no game files, any Python.

The C file's stand-ins: the hook as `vk_dlaa_pass` keeps the light target (the target of the last pass that ran it); a
group as `vk_exec.c` keeps it (whether its first chunk began a pass); `pass_begin()` is a pass begin of a thread that
records no chunk (group NULL), or of chunk `idx` of a group, into a target.

- The rule itself.
- One thread: a pass begun twice (after a barrier) changes nothing, the last pass of the stream keeps the light target.
- A group whose chunks the workers record while the submitting thread waits for it: its passes begin in any order after
  the first chunk's (the first chunk's pass again, after a texture lookup waited for its turn), and the pass after the
  group comes after all of them.
- The submitting thread goes on past the group: the scene pass after it begins once the group's first chunk began its
  pass (`vk_exec_hooks_wait` let it through: the group's hooks ran), and that chunk begins its pass again after that
  (late: after a lookup's turn, a barrier, a pass of the replay's own). The scene pass is later in the stream and keeps
  the light target; the hooks running at the second begin gave the upscaler the earlier pass's image for a frame.
- Two groups out and the pass after them: each first chunk begins its pass after the group before began its own
  (`chunk_record`), and each begins it again out of order.
- A first chunk whose first pass begin waited (a deferred lookup returns before the hooks): the hooks run at the begin
  that goes through, which is the one that marks the group.

## `tests/pb_torn_check.py`

Checks how `pipeline-binaries.bin` keeps its whole records (`runtime/vk/vk_pipeline.c` `pb_index()` and `pb_keep()`,
`NX2SARIA_PIPE_BINARIES=1`). `pb_init` keeps the whole records of a file that ends inside one (the process ended
during an append, or a disk filled) or holds a malformed one, and cuts the file back to them; the walk must say where
the last whole record ends, and `pb_keep()` must cut the file there. The check cuts the file's record layout and
`pb_hash`, `pb_find`, `pb_insert`, `pb_index` and `pb_keep` out of `vk_pipeline.c`, writes files of whole records, one
with a torn last record and one with a malformed record in the middle, and checks the offset and the index the walk
gives, the length `pb_keep()` leaves on the disk, that a second walk reaches the end of the cut file, and that a
record appended after the cut is read back. `plat_file_truncate` is a stand-in that cuts the file with the C library.
No GPU, no game files, any Python; it needs the Vulkan headers of MSYS2 UCRT64 (`vulkan/vulkan.h`).

## `tests/perf_bound_check.py`

Builds and runs `tests/perf_bound_check.c` on `runtime/diag/perf_bound.h`: which side holds the frame back in the
60-frame window line of the perf trace (`runtime/diag/perf.c` `perf_gpu_line()`): pace, gpu, serial or cpu. It needs a
C compiler only: no Vulkan headers, no GPU and no game files.

- Kokiri Forest on an RX 6700 (RADV, 2026-11-19, 0.19.0-rc.2): 33.6 ms frames, GPU busy 22.5 ms, idle waiting for the
  next submission 9.6 ms, the game's threads waiting 17 ms for GPU fences. The old rule said cpu.
- The same window without the idle split (no calibrated timestamps): the rule cannot see the turns, cpu as before.
- A full GPU wins over serial. Pacing wins over everything.
- The CPU alone: the GPU starves but nothing waits for it.
- Threads wait for the GPU but the GPU always had work queued: not taking turns, and not full: cpu.
- The edges are inclusive: a fifth starved and a quarter waiting.
- Without timestamps the fence waits decide, as before.
- An idle split with no interval (the first window of a run): no division by zero, cpu.

## `tests/perf_summary_check.py`

`tools/perf_summary.py` reads the present side of a log: lines made with the runtime's format strings give the present
percentiles, the present thread's window averages and the render thread's hand-off share, and the lines of older logs
(no hand-off share on the perf line, no present call or hand-off on the `present:` lines) still parse.

The format strings are copied into the test and must be kept in step with the runtime:
`runtime/vk/vk_backend.c` `present_main()`'s `TRACE=present` line and `present_window()`'s line, as the C source has
them, and `runtime/diag/perf.c` `perf_present()`'s window line, its head to the shares (the test fills in the rest).
`source_has()` checks that they are still the runtime's; the C sources split them over string literals, so adjacent
literals on following lines are joined first. `c_fmt()` turns a C printf format with `%llu`, `%lld` and `%u` into
Python's `%d`, `%%` kept.

The sample: 100 presents of the new form with intervals 16.0..16.99 ms, then one of 40 ms; the present call 0.1..1.09
ms; the hand-off 2.0 ms each but one of 30 ms. Of the 101 intervals p50 is the 51st smallest (16.50), p99 the 100th
(16.99), max 40.

## `tests/pgo_build_test.py`

The profile-guided build of `tools/make_game.py` and `tools/pgo.py`, with a stand-in for the compiler process. No game
files.

Which flags a build compiles with: a kit without an execution profile (the flags and keys it always had), a kit with
one that fits (the lifted code at -O2 and every compile with the profile), a record that names another `recomp/`, a
record that is missing or names none, a profile the compiler refuses in the trial compile, `NX2SARIA_PGO=off`, and an
instrumented build (`NX2SARIA_PGO=gen`). The object keys: the same profile in a kit at another place keeps them,
another profile changes them. `tools/pgo.py`: the record a merge writes names the `recomp/` a kit compares, a game
built from another `recomp/` stops the merge, and a release refuses a profile of another tree.
`tools/check_content.py` refuses profiles in a commit.

- `Compiler` stands in for `subprocess.run` in `make_game.py`: it records every command, answers `--version`, writes
  the object and the dependency file of a compile, and refuses a command with `-fprofile-use=` when `refuse` is set.
- `PgoBuildTest.add_profile()` puts a profile in the kit's `data/` with a record that names the kit's `recomp/`
  ('fits'), another one ('other'), none ('empty'), or without a record (None). `PgoBuildTest.commands()` gives (the
  lifted code's command, a runtime file's) with the flags, without the compiler and the paths (up to
  `-I <runtime> -o <object> <source>`). `PgoBuildTest.make_real_recomp()` makes the test kit's `recomp/` this
  checkout's, so that what `make_game.py` records in the kit is what `tools/pgo.py`, which reads the checkout,
  compares it with.
- Without a profile there is nothing to try: no compiler runs, `pgo_tag()` is empty (the compile step's key is what it
  was) and `pgo_salt('compiler', None)` is `'compiler'` (so are the object keys: the flags and the compiler's id
  alone, the key a kit before profiles gave).
- A fitting profile: one trial compile by each compiler that will read the file, the fast one with its sysroot. A
  source checkout takes the profile from `build/pgo`, but a release's `data/` comes first.
- A record of another tree leaves the profile out, decided before any compiler runs; the same record fits once the
  kit's `recomp/` is the one it names, and not after the lifter changed again.
- A profile the compiler refuses is left out: one compile says so, not every compile of the build; the second
  compiler's refusal counts too.
- `--pgo off` (`PGO_MODE` 'off') leaves a fitting profile out; `NX2SARIA_PGO` with a value that is no mode is as
  without it.
- An instrumented build counts into the game folder: what the game was built from goes beside the counts, and
  `tools/pgo.py` reads it back. Build tools without the profile runtime stop an instrumented build; with the runtime
  there is nothing to say.
- A launcher update puts the kit into another folder: the same profile there keeps every object. Another profile, or a
  build that leaves it out, compiles again (four objects each time: no profile, the kit brought one, another, left
  out).
- `PgoToolTest.profdata()` stands in for llvm-profdata. The merge's record is what `make_game.py` works out for a kit
  with this checkout's `recomp/`: the profile fits such a kit.
- `RealCompilerTest` uses the real compiler when MSYS2 UCRT64's clang is installed (and an llvm-profdata, when one is
  found): a file that is no profile (a profile's first bytes and nothing after them) is refused by the trial compile,
  a real one is taken. `TEXT_PROFILE` is one function with one counter, as llvm-profdata reads text.

## `tests/pipeline_cache_file_check.py`

Builds and runs `tests/pipeline_cache_file_check.c` on `runtime/vk/vk_cache_file.h`: the read of `pipeline-cache.bin`
(`runtime/vk/vk_pipeline.c` `cache_load()`) with 64-bit sizes. The C program makes a sparse file just over 2 GB in the
folder given as `argv[1]` (`build/test/pipeline_cache_file`; it takes no disk space, and about 2 GB of memory for the
seconds the read lasts), with a marker at its start and at its end, and the Python side removes it. `HEADER` (32) is
the size of the header every pipeline cache starts with (`VkPipelineCacheHeaderVersionOne`). The file helper writes
`head` at the start, `tail` as the last bytes, and a hole between them.

- No file: a first run, which is not a refusal. A file of a header alone holds no pipeline: refused with its size. A
  small file: read whole.
- Just over 2 GB: a 32-bit tell measures it as negative. Under a limit it is refused before any read, and its real
  size comes back for the log line; with no limit, and with a limit above its size, it is read whole.

## `tests/pipeline_list_test.py`

`tools/pipeline_list.py` on made-up game folders: records folded once, a key whose shader the folder's answers do not
name left out, the compute variants taken, a list made before as an input, `--mesh-only`, a folder of another record
layout left out, the file read back, and what `tools/prepare_shaders.py` takes from a list for its seed. Also
`tools/check_content.py` refusing such files in a commit. No game files; stdlib only.

`REC` (464) is a `KeyRec` of the runtime that wrote the test's files: three ids, two flags, the state. `answer()` makes
a `shader-answers.bin` record: stage, code hash, input id, 147 bytes of answers, the pad byte. The file the tool
writes is the layout the runtime reads as it is (`pipeline-keys.bin`). `PipelineList.test_seed_variants()`:
`prepare_shaders.list_variants` gives the variants of shaders the game version has, once, with the id left to the
runtime; the case is skipped on an `ImportError`, since its shader inventory needs Python 3.14's `compression.zstd`.

## `tests/platform_bulk_write_test.c`

`plat_mem_prepare_write` on Linux (`runtime/platform/linux.c`): every page of a prepared range counts as written, a
reset leaves it clean, and a later store to one page is still seen. No game files. It is built and run by hand from
the repository root, and exits 77 when the write watch is not available on this kernel:

```sh
clang -O2 -ffunction-sections -fdata-sections -Iruntime -c runtime/platform/linux.c -o /tmp/plat.o
clang -O2 -Wall -Iruntime tests/platform_bulk_write_test.c /tmp/plat.o -Wl,--gc-sections -lpthread -ldl -o /tmp/bulk
/tmp/bulk
```

## `tests/platform_dmabuf_test.c`

Guest memory as a memfd's (`runtime/platform/linux.c`, `NX2SARIA_DMABUF`): a dma-buf of a watched range maps the very
pages the CPU writes; a decommit inside it zeroes them in place (the dma-buf still sees the CPU's pages), one outside
it reads zero when committed again; the write watch keeps working. No GPU, no game files. Built like
`tests/platform_bulk_write_test.c`, with this file and `-o /tmp/dmabuf`; exit 77 means no `/dev/udmabuf` or no write
watch here. `memfd_bytes()` is the guest memfd's allocated bytes.

- The write watch is read and reset until a call has fewer than it could take.
- A dma-buf of the first 8 MiB maps the guest's own pages; a write through it, as a GPU write, is seen by the guest and
  not by the write watch.
- Protection changes (a freeze, an armed lazy range) leave the dma-buf alone.
- A decommit inside the dma-buf: zero in place, the same pages for both. One that straddles the dma-buf's end: zero
  both sides when committed again. One outside any dma-buf: the pages leave the memfd and read zero.
- A dma-buf over pages nothing committed yet allocates them; when it is gone they go back, the committed ones stay.
- Outside the reservation, or not whole pages: no dma-buf.

## `tests/platform_linux_test.py`

Fast checks for Linux platform services; no display, game data or build tools required. The repository root goes on
`sys.path` for `launcher/`, when the file is run as `python tests/platform_linux_test.py`.

- `UserDirsTest`: the desktop's own Downloads, Desktop and Documents folders (`user-dirs.dirs`), which a desktop set
  up in another language translates: the dump search and Export Save look there too (`launcher/winsys.py`
  `known_folder()`). A folder set to the home folder is turned off; with no file there, none.
- `OtherDrivesTest`: the dump search also looks on the disks mounted under the system's mount folders (a partition
  shared with Windows), and the dump and folder rows open the desktop's chooser, which reaches them. Only local disks
  mounted now count: an autofs placeholder is not listed (listing it would mount the disk) while the disk itself,
  mounted, is; a placeholder not mounted now, a network share and another account's removable disk are left alone;
  SteamOS's SD card counts; the mount folder itself is no drive below it. The search goes two levels into a drive (a
  file three levels down is not looked at; the report counts home, the drive, its folder and the 2 below) and stops in
  time.
- The desktop's chooser: a name that is no UTF-8 comes back as it is; a cancelled zenity does not ask kdialog too; on
  KDE its own comes first; a broken zenity (255) falls to kdialog. The chooser says in the log which it was and what
  it gave. "All files" stays in the filter: the desktop's patterns are case-sensitive, and `GAME.NUP` would not match
  `*.nup`.

## `tests/platform_restart_mask_test.c`

`plat_restart` on Linux (`runtime/platform/linux.c`): the game it starts may run on every processor this one could
before `plat_cpu_reserve` took a core for the recording thread, whichever thread asks for the restart. No game files.
Built like `tests/platform_bulk_write_test.c`, with this file and `-o /tmp/restart_mask`; exit 77 means this machine
has no core `plat_cpu_reserve` would take. The test starts itself again through `plat_restart`; that copy (it sees
`NX2SARIA_RESTART_WAIT`) only writes how many processors it may run on, and the test waits up to 5 s for what it
wrote (-1 when it never came). The restart is asked for from a thread that existed before the core was taken, as the
game's main thread does, and from the thread that holds the core.

## `tests/platform_stand_ins.py`

Not a test: the stand-ins the tests that link the platform layer alone build with (`tests/platform_stand_ins.c`), so
they need neither the rest of the runtime nor SDL3's development files: the platform layer's own calls out are
answered there. `stand_ins()` gives the compiler arguments that add the stand-ins to a build in the folder `work`: its
include folder and source; `include()` those for the stand-in SDL3 header, written under `work`, the one SDL3 header
`runtime/platform/linux.c` includes, with the calls it makes.

`tests/platform_stand_ins.c` stands in for what the platform layer (`runtime/platform/linux.c`,
`runtime/platform/win32.c`) calls outside it: the settings' environment read (`runtime/cfg/cfg.c` `cfg_env()`), as
`cfg.c` has it for a name outside the settings' prefixes (the environment's own value), and, on Linux, SDL's clipboard
(`plat_clipboard_set` / `plat_clipboard_get`): no clipboard, so a set fails and a get finds nothing.

## `tests/play_gate_check.py`

Builds and runs `tests/play_gate_check.c` on `runtime/vk/vk_play_gate.h`, the play gate of the background pipeline
builds (`runtime/vk/vk_pipeline.c` `play_rest()`): they rest while the frame shown in play has `bg_build_play_draws`
draws or more and was shown within half a second, never in a light moment (an area load, the boot screen's warm, a
preparation) or with the setting at 0; and what the log's line "background builds paused in play: N frames, M builds
deferred" counts. Without the differential tests' packages (an `ImportError`) it makes the same compiler choice
without them.

What the builders do at rest, and what it changes in the frame rate, shows only in a run: the start's line "background
builds rest in play", the line above at each save in play, and [performance.md](../performance.md) ("Background builds
in play") for a measurement with `tools/bench.py`. `LOG_LINES` are the log's lines a run is checked by, as the formats
in the runtime's source; `log_lines()` returns 0 when each is in the runtime's source as its format string, adjacent
string literals read as one.

- The game in play: a frame of 2,850 draws shown 16 ms ago. Builds rest. At the threshold they rest, one draw under it
  they go on: a menu, the pause screen, the title.
- No frame shown for `PLAY_GATE_SHOWN_MS`: a load or a hang, not play. Builds go on.
- A light moment whatever the frame: an area load, the boot screen waiting for the warm, a preparation.
- The setting at 0: they build in every frame, as before. A setting of 1: every frame shown in play is heavy.
- The counts: a session where two builders rest over three heavy frames with 5 queued builds, a third queued during
  the rest, then a light frame lets them go on. A heavy frame while nobody rests yet is not counted; the first builder
  finds the queue's 5 builds, the second finds them counted already; a light frame is not counted.
- The line comes when the counts moved, and once for them. More ends than begins never leave a negative count of
  builders at rest, which would count no frame again.

## `tests/present_held_check.py`

Builds and runs `tests/present_held_check.c`, with `stand_ins()` of `tests/platform_stand_ins.py` for what the
platform layer calls outside it: `runtime/pacing/vk_present_held.h`, the present that waits for held work, with one
presenting thread and three submitting threads over 200,000 frames, in which each reason a held present goes out must
occur. No GPU, no game files, any Python. Every frame must reach the present callback exactly once and in
frame order, never two callbacks at once, and a submitting thread must never wait for the lock. The argument `old`
runs the unlocked check-then-clear of 0.10.1 (`vk_backend.c` `ph_flush` before `vk_present_held.h`) through the same
harness and only prints what it sees, to show that the harness catches the race.

- The stand-in for `runtime/vk/vk_backend.c` `ph_out()`: `present_image`'s work is the spin, the window a second caller
  would fall into. The stand-in for `ph_why_after()`: out when nothing is held any more, or `PH_MAX` submissions after
  it began to wait.
- `stress()`: `FRAMES` (200000) is the least; the stress goes on until both threads have put held presents out. Held
  presents went out from a submitting thread (after the work or at `PH_MAX`) and from the presenting one (at the next
  present), so the lock was in the way of both. How soon that happens is the scheduler's: the stress runs until it has
  (`FRAMES_MOST` at the most), not for a fixed count. `PH_MAX` alone takes 8 submissions in a row that leave work
  held, which a machine with few cores schedules rarely: `steps()` checks it. In the loop an odd step clears `held`
  (`held_resume` ran what it could), and the other branch is `vk_be_present`'s synchronous one.
- `steps()`: one thread, step by step. `PH_MAX` submissions after it began to wait put a present out though work is
  still held.
- `contended()`: the lock held elsewhere: a submitting thread does not wait, the presenting thread waits and is
  counted (the present is armed, another thread holds the lock and puts the held present out).

## `tests/preset_check.py`

Builds and runs `tests/preset_check.c`: the quality presets, `runtime/cfg/preset_pick.h` (the tier made-up hardware
gets, the handheld names, what each tier sets) and `runtime/cfg/cfg.c`'s preset layer (`cfg_start`'s choice, the state
file, the menu's calls), with stand-ins for what `cfg.c` links against (`tests/preset_check_stubs.h`). No GPU, no game
files. The C program has three modes:

- `pure`: `preset_pick.h` alone; prints "ok:" or "FAIL:" per check. The tier made-up hardware gets (the bench PC's RX
  6700 with 10 GB, 16 threads and 32 GB is high); the three PCs [presets.md](../presets.md) names: the bench PC, a
  laptop (its log: 6017 MiB, RAM 16087 MiB) and a player's RX 6650 XT with a Ryzen 5 4650G, whose integrated graphics
  keep part of the 16 GB. Ultra is never recommended: any type, mesh shaders or not, up to 96 GiB of video memory, any
  RAM and processors. What the GPU runs; the handheld names, as Linux's DMI tables and Windows' registry give them;
  what each tier sets, over a spread of hardware (high nothing, no tier frame generation, DLSS and FSR 4 only below
  high and only on the cards that run them, a frame-rate limit only on an integrated GPU or a handheld and never above
  60); fingerprints.
- `values <dir>`: every value any tier gives any key on any hardware, read by `cfg.c` as the settings file's and as a
  Ctrl+F5 reload's; prints "FAIL:" for a value outside its setting's choices. The values do not depend on the
  hardware, only whether a row applies. `cfg_load` logs a value outside its choices; then a reload of a file without
  it and with it shows what a reload accepts (`cfg.c` `value_ok`).
- `run <dir> ...`: `cfg_init` and `cfg_start` with the settings file and state file in `<dir>` and the hardware in
  `NX2SARIA_TEST_HW` (with `NX2SARIA_TEST_TOOL` set, as a tool mode starts: `runtime/main.c` `game_run()`), then the
  actions after it: `use=<tier>`, `chosen`, `set=<KEY>=<value>` (`cfg_set`, as the menu's Apply does), `save` (the
  menu's save after its Apply), `ini=<text>` (the settings file's new text, for a reload after it), `reload`, `notice`
  (the menu shows the notice when it is due), `handoff=<tier>` (the menu's Apply of a preset choice: the values
  before, the keys set by hand reset, the switch), `device` (the game has chosen its Vulkan device,
  `hw_detect_device_check`). It prints every line `cfg.c` logs ("log: ...") and what the readers get ("got: ..."),
  among them the keys choosing the tier in use in the menu hands back (`runtime/ui/option_rows.cpp`
  `opt_preset_handback()`, outside a draft): the keys it sets that the player sets to another value, the environment's
  aside.

What `tests/preset_check.py` `main()` runs through `run` (`run()` there starts in a folder of its own, or in one kept
from an earlier run; `get()` reads what "got: <what> ..." says in the n-th report, 0 after the start, 1 after the
first action):

- The layer's place: the player's keys, the environment's first, win, the device profile next, then the preset, then
  the table. The device profile's value wins over a tier's for one key through a test-only profile row
  (`CFG_TEST_DECK_ROW`): the Steam Deck's value for a key Low also sets, with another value (`standin_filter` easu,
  Low bilinear).
- The bench PC: high, which sets nothing; the state file is written once, and not again while nothing changed (only
  when its text changes, never `nx2saria.ini`; blank lines; a GPU missing a line).
- A GTX 1650: low; a key of the settings file wins over the preset and makes it custom. An ordinary setting of the
  environment beats the preset, is never handed back, and makes it custom.
- The menu's switch: values and their log lines, restart-only ones pending, the version bumped.
- A tier the player chose: kept, recorded as chosen on this PC, logged against the recommended one. New hardware: the
  chosen tier gives way to the recommended one until the player chooses again (in the menu or by editing the line,
  Ctrl+F5). A state file of the older form, its fingerprints in MiB, on the same PC. A tier in the environment wins
  over the new-hardware rule. Ctrl+F5 after an edit of the preset line on new hardware: the player's choice here.
- No probe (`preset_probe` = 0, or one that cannot run): the last probe's GPU, else nothing known (high). A probe that
  never returns (a driver's crash): the probing mark stays, and later starts go by the last result.
- RAM caps: an RTX 4060 with 12 GB is medium, through DLSS quality.
- The laptop and its settings file: its own lines keep their values, and a preset chosen in the menu hands back
  exactly the ones a tier sets on this card (`fps_limit`, `framegen` and `upscaler` are no preset's here).
- The Steam Deck: low below the device profile.
- A value that is no tier, and a reload of the preset line. Ctrl+F5 after the menu switched: the file's line against
  the one in memory, not the tier at start.
- A tool mode: no probe, the last probe's GPU, no state file written. A state file of garbage: read without harm, no
  GPU taken from it.
- The one-time notice (`cfg_preset_notice_due`, the state file's noticed line): due while the player never chose and
  this PC gets another tier than high; once.
- The menu's calls (`cfg_preset_use`, `cfg_preset_chosen`, `cfg_preset_custom`, `cfg_preset_use_from`,
  `cfg_default_tier`, `cfg_preset_differs`); the menu's Apply of a preset choice: defaults of a tier, keys that
  differ, and the log from the old values.
- The menu's draft (`tests/preset_draft_check.cpp`, built by `build_draft()` with `runtime/ui/option_rows.cpp`,
  `runtime/ui/ui_text.c`, `cfg.c` and Dear ImGui, whose objects are kept in `build/test/preset-imgui` between runs):
  a staged tier's values in the rows (`opt_value`, `opt_get`, `opt_default`), the handback (only the keys the chosen
  tier sets, to another value than the player's), and Apply (`opt_draft_apply`). Low, Ultra, Low again: Ultra hands
  back the `game_res` = 1080 the player set, and Low gives it 1080 again. Apply removes the line but neither counts
  nor lists it as a change, nor says it waits for a restart.

## `tests/preset_check_stubs.h`

Stand-ins for what `runtime/cfg/cfg.c` links against, for the quality preset tests: `tests/preset_check.c` (C) and
`tests/preset_draft_check.cpp` (C++, with `runtime/ui/option_rows.cpp`). It defines them: include it in one file of a
program, after `cfg.h`, `hw_detect.h` and `platform/platform.h`.

`rt_log` prints "log: <line>"; `plat_file_write_atomic` prints "write: <path>" and writes the file in place; a thread
(`plat_thread_spawn`) runs its function at once on the caller's, so a test sees what it wrote when the call returns.

`hw_detect()` gives the hardware `NX2SARIA_TEST_HW` describes, "field=value" between commas: `name`, `vendor`,
`device`, `driver`, `type` (discrete, integrated, cpu, other), `vram` (MiB), `mesh`, `cores`, `threads`, `ram` (MiB),
`hand` (a handheld's name), `skip` (the probe does not run: the GPU fields stay empty for `cfg.c` to fill from the
state file, which is what the real `hw_detect` leaves), `win` (FSR 4 counts), `crash` (the probe "crashes": the
process ends with code 9 right after `before_probe`, as a driver's crash would end it). Without a name, no GPU. A
`no_probe` reason from `cfg.c` skips the probe as the real one does; else `before_probe` runs first.

## `tests/preset_draft_check.cpp`

The quality preset in the settings menu's draft: `runtime/ui/option_rows.cpp` (`opt_value`, `opt_get`, `opt_default`,
`opt_is_set`, `opt_preset_tier`, `opt_preset_handback`, `opt_draft_apply`) over `runtime/cfg/cfg.c` with made-up
hardware, no window and no GPU. Built and run by `tests/preset_check.py`; `option_rows.cpp`'s rows draw with Dear
ImGui, but nothing here draws. The stand-ins for what `cfg.c` links against are in `tests/preset_check_stubs.h`; the
menu's skin, sounds and the renderer's restart questions, what `option_rows.cpp` and `ui_text.c` ask of the rest of
the game, are stand-ins in the file. `PerfSizes` is only declared: it is `host.h`'s, a C-only header that pulls in
`cpu.h`, and the stand-in for `perf_size` only passes a pointer on.

`preset_draft_check <dir> <action>...` runs `cfg_init` and `cfg_start` with the settings file in `<dir>` and the
hardware in `NX2SARIA_TEST_HW`, then each action:

- `stage=<tier|recommended>`: what `runtime/ui/menu.cpp` `preset_stage()` does when the player picks that entry of the
  Quality preset row: the preset key, then "" for each key `opt_preset_handback` hands back (`tier_of()` gives -1 for
  recommended).
- `set=<KEY>=<value>`: a row writing a key (`opt_apply`; "" unsets it).
- `apply` / `discard`: `opt_draft_apply` / `opt_draft_discard`.

After the start and after each action ("--- after <action>") it prints "got: ..." lines: the staged tier, the handback
for every tier, and for each key of `cfg_preset_keys` what `opt_value`, `opt_get` and `opt_default` give.

## `tests/private_paths_test.py`

`tools/private_paths.py`: the account's folders written short in what the launcher and the build tools write for
people (`launcher.log`, `crash.log`, Copy Report, the AppImage's `build.log`, the bug report zip), with the same rules
as the game's log (`runtime/log_privacy_core.h`, `tests/log_privacy_check.py`).

Windows and Linux rules on any system (an environment and a system given to `Shortener`), accents and spaces in the
account's name, an account whose folder starts the same, a domain profile (the folder `<name>.CORP` keeps its
suffix), the name alone inside a path but not inside another word nor in text without a path, the switch
`NX2SARIA_LOG_SHORT_PATHS=0`, and the launcher's log and bug report zip. Many names in a long text: each one reads
only the run around it (a bug report's 64 MiB log).

## `tests/prompts_record_check.py`

Builds `tests/prompts_record_check.c` with `runtime/input/input_map.c` and runs it on a temporary folder: the game
folder's record of where its button prompts came from (`button-prompts.txt`; `runtime/input/input_map.c`
`input_prompts_record()`, which `runtime/hle/romfs.c` `overlay_init()` logs as "button prompts: ...";
`tools/make_game.py` `record_prompts()` writes the file). The reader gives its first line without the line end, cut
short to the buffer (a line longer than the buffer ends inside it), and 0 for a folder without the record or with an
empty one. No game files. The stand-ins are what `input_map.c` needs from the rest of the runtime; none of it is
reached from `input_prompts_record()`. `runtime/simd_fp.h` `FOK` ors two compares on purpose, and clang 18 warns on
it. The `--help` text is the string `DESCRIPTION`.

## `tests/prp_file_check.py`

Builds and runs `tests/prp_file_check.c` on `runtime/gfx/prp_file.h`: the layout of `shader-prepared.bin` with what
the list was prepared from. The C side checks the parse and then writes a version 2 list into the folder given as
`argv[1]`, whose head `tools/make_game.py` `prepared_from()` then reads, so the game's writer and the build's reader
cannot drift apart. No game files, no GPU.

- Version 2: the head, then three records. A cut last record is not counted. The head alone, or less: no list.
- Version 1: records right after the 16 bytes, and nothing about where they came from.
- Another version, record size or magic: not read.

## `tests/pause_tab_check.c`

The settings tab in the game's pause menu without the game (`runtime/ui/pause_tab_core.h`); `tests/pause_tab_check.py`
builds and runs it, and with `--romfs <folder>` reads the game's own layouts (`tools/layout_dump.py`) and checks that
the panes the tab writes and the parents it relies on are as `PT_SYSTEM_TREE` and `PT_CATEGORY_TREE` say, and that
`MenuOverlay_00` has four categories.

- R: held from before the arming goes to the game; a new R while armed opens and is taken until let go.
- The tab's box: past the game's R icon at 16:9 and with the categories spread, as large as a category; none from
  categories in the wrong order or empty.
- The page is settled only in the middle and still.
- The panes' shapes, a changed shape refused, too few panes refused.
- The name setting: unset and `product`, `settings`, an unknown value.

## `tests/quit_menu_check.c`

Quit Game in the game's own menus without the game: the message file edit, the window's labels and the press
(`runtime/ui/quit_menu_core.h`), and with a game folder the edit of its message archives
(`runtime/ui/quit_menu_text.c`); `tests/quit_menu_check.py` builds and runs it, with `--romfs <folder>` for the game's
archives. The message files of the first part are made by the test from the format.

- The label hash puts the labels of two of the game's files in the groups the game has them in (`0000`, `0002` and
  `0001` on two groups; `1000`, `0000`, `1001`, `0001`, `1002` on five).
- A file made by hand reads back; a text with a control tag, whose bytes hold zeros, is whole.
- Labels added: the new ones are found, the old ones keep their texts, the size field and the 16-byte padding are
  right; a label that has its text already changes nothing and makes no file; one with another text is replaced; five
  labels into an empty file of five groups each land in their group.
- Refused: UTF-16, a third section, a file cut anywhere, a text without its zero.
- The window's labels with three buttons, and every other label left alone.
- The press: A on the entry is taken and chosen once, stays taken while held when the focus goes, and an A held from
  another entry chooses nothing.
- With a game folder, for each message archive: the three labels have the texts of the labels they copy, the window's
  own labels keep theirs, exactly one member of the archive differs, and an archive that has the labels needs no
  edit.

## `tests/quit_menu_sites_check.py`

The set `quit_menu` of `recomp/patches-v111.txt`: its name, switch and default, so that `NX2SARIA_QUIT_MENU=0` leaves
the game's own word at every site; its seven sites and no other; each site's original word and the shape of its
replacement (plain words, then a call of the page's +0xee0, +0xe20 or +0xe10, or of the game's helper at
`main+0x3405bc0`); the words each pair of cuts puts back after its call; the one site it shares, with `menu_mouse`,
making the same call; no other set calling its two slots; `runtime/ui/quit_menu.c` listing the same words for its
check at start; and the three host functions registered at their slots (`runtime/patches/patches.c`).

## `tests/rebrand_compat_test.py`

Branding transitions preserve update discovery, private reports, shortcuts and existing installs. No game files.

- The launcher renames a legacy settings file; with both there, `nx2saria.ini` is the file and `nx2navi.ini` is left as
  it is; a fresh folder gets `nx2saria.ini`, nothing renamed.
- Both update manifest names discover current payloads: the test uses the Windows manifest's two names, so the feed
  looks as on Windows; Linux has a manifest of its own.
- Both PGO build flags keep the runtime flush; `runtime/platform/linux.c` includes SDL3's clipboard header, for which
  the stand-in `tests/platform_stand_ins.py` writes will do.

## `tests/release_files_test.py`

`tools/release_files.py`: what the release workflow (`.github/workflows/release.yml`) refuses before it attests and
publishes, and the release page text. Standard library only, so the content guard workflow runs it. `release()` makes
a folder with the files the Windows and Linux builds hand to the release workflow. A good release gets its sums, also
on a second run over its own `SHA256SUMS.txt`; a pre-release's body holds "Pre-release" once, the notes' own line
only, no second one on top.

## `tests/replay_hazard_check.py`

Builds and runs the replay hazard probes on the real runtime sources, on Linux, without game files or a GPU:
`python tests/replay_hazard_check.py --vma-include <vma-dir> --work <scratch-dir> [--tsan]`. The `--help`
description is the string `DESCRIPTION`.

- `tests/usnap_order_check.c` (`runtime/vk/vk_usnap.c`): the order of uniform snapshot drops between replay chunks.
  Barriers force a drop to run after a later chunk's lookup (stale) or before an earlier chunk's lookup (early); each
  forced order must read the wrong value and each control the right one. With `--tsan` it is also built with
  ThreadSanitizer, which reports nothing there: the barriers order every access, and the fault is the order itself.
- `tests/vkmem_lifetime_check.c` (`runtime/vk/vk_memory.c`, AddressSanitizer): a pool record used across the pool's
  finalize, and mirrors used while idle ones are evicted. The protocol as it is must hold; its controls, with the
  protection taken out, must fail (a use after free, a use without a buffer).

`vk_memory.c` includes the runtime's shader headers: build them first (`python tools/build_shaders.py`, into
`runtime/`) or pass a build's folder that has them with `--spv-include`. The bench uses Clang 20; pass its path with
`--cc` if needed.

## `tests/romfs_check_test.py`

`tools/romfs_check.py` on a small made-up game: damaged files are found, a damaged texture gets a stand-in with its
own name inside, and a clean copy leaves nothing behind. No game data. `sarc()` writes a SARC archive: little endian,
the file table, then the names, then the data.

## `tests/romfs_packer_test.py`

RomFS read-ahead and packing with synthetic files. In the layout case the read-ahead limit is lowered so the tiny
files span several worker groups.

## `tests/romfs_prompts_test.py`

`tools/make_romfs_patches.py --prompts` and `tools/button_icons.py` on a RomFS made in the test, with no game files:
each prompt names the label of the Xbox button that the pad layout presses; the shoulder buttons, triggers, Plus,
Minus and the d-pad are drawn as on an Xbox controller; the PC prompts draw every button as its key or mouse button
(the defaults or a `nx2navi.ini`'s `bind_` lines); the resource size table grows with the fonts; every other file and
member stays byte for byte; the same input gives the same output; and a RomFS the tool does not recognise fails before
it writes anything. Both game versions: 1.2.1 (`Common.Product.110`, 72x72 button guide images) and 1.4.3
(`Common.Product.140`, 108x108). It runs with the Python 3.14 environment.

Tables at the top: the face button each Switch button is in a pad layout ([controls.md](../controls.md),
`runtime/input/input_map.c`), the label an Xbox pad prints on it, and the layout each overlay is made for
(`runtime/input/input_map.c` `input_prompts_folder()`); the glyphs drawn as Xbox icons, with their advance (LB RB LT
RT, Menu and View, the d-pad); the resource size table's name after its version, in both forms the tool takes.
`IMAGE` (16384) is an R8 square up to 128 rows, two GOBs wide, block height 16: the size of the game's key images.

The formats are written in the test from their layout, so the tools' readers are checked against an independent
writer:

- `bflan()`: a FLAN whose `pai1` section steps each pane's visibility from 0 at frame 0 to 1 at frame 1;
  `visibility()` reads pane -> [(frame, value)] back.
- `bntx()`: a BNTX of single-mip square textures, name -> (format, size, image), block height 1 << 4 GOBs;
  `bntx_images()` reads it back, names without the `^` suffix.
- `bflyt()`: a FLYT with one `txt1` section per pane (name -> text) after a `pan1` section.
- `msbt()`: a UTF-16 MSBT: one `LBL1` group with every label, then `TXT2` (label -> text).
- `restbl()`: a RESTBL keyed by the CRC32 of each path without `.zs` (path -> size).
- `bfotf()`: a `.bfotf` as `tools/button_icons.py` `bfotf_encode()` writes it: 32-bit words XORed with the key; its
  first word is the key XOR `MAGIC` (0x18029a7f).
- `rect()`: a rectangle, counter-clockwise (filled, the CFF way) or clockwise (a counter). `cff_font()`: a CFF OpenType
  font with a 1000-unit em: code point -> (advance, draw(pen)). Every button glyph gets a distinct rectangle and
  advance, so a glyph that moved is found by its outline. The other characters the PC prompts' keys show
  (`button_icons.PC_LABELS` of the defaults) are a block with a counter, each its own size. `PC_PANES` holds the
  diamond's letters with the default keys.
- `Romfs`: a RomFS (`romfs.bin`, `index.json`) holding the files `make_romfs_patches.py` reads (RomFS path -> contents
  before compression), the originals kept to compare. An older version's resource size table is there too and is left
  alone. The same font sits under another key: drawn once, stored under each archive's own key. The regional font and
  the two tables are stored without a dictionary. `Romfs.read()` gives the overlay's file, decompressed. `glyph()`
  gives (outline, advance) of the glyph of a code point.

`PromptsTest.each()` yields (RomFS, style, overlay folder, summary) of every run of the styles. The face buttons name
the Xbox label of the button pressed (Switch button -> the label of its button); text that only looks like a letter
pane, and every other message, stays. In the Xbox icons the letters are holes in the key: LB is the key, L, and B with
its two counters; RT is the key, R with its counter and T (four contours); d-pad up is the cross and the arrow (two).
The text fonts and the archives' other members stay byte for byte; both archives hold the same font. The button guide
images show the same icons, a margin around a white key; the 60 fps patch and the prompts touch only these members.
The PC prompts hold the left stick's W A S D (0xE05E), and a key's label is a hole: E is the key, the block and its
counter.

## `tests/rt_knob_check.py`

Builds and runs `runtime/diag/perf.c`'s `rt_knob` (cut out of the file with its warning table: `cut()` takes `perf.c`
from the warning table's `#define` to the end of `rt_knob`) with stand-ins for the settings and the log. 0, 1 and
`ab` read as before and log nothing; `false`, `off` and `no` in any case read as 0 and `true`, `on` and `yes` as 1,
without a line; any other value, such as "0 ; a note" from the environment (the settings file's loader cuts that
comment) or "enabled", reads as 1 and is logged once per switch, however often the switch is read. No GPU, no game
files, any Python.

## `tests/rtss_limit_check.py`

Builds and runs the checks of `runtime/pacing/rtss_limit.c`, RivaTuner Statistics Server as the frame limiter
([building.md](../building.md#checks) lists the scenarios). `tests/rtss_limit_check.c` builds
`runtime/pacing/rtss_decide.h` alone, the parts that make no system call, on any system. `tests/rtss_worker_check.c`
(Windows only, skipped elsewhere) compiles the real `rtss_limit.c` against a fake RTSS and runs each of its scenarios
in a process of its own. The Python file checks statically that a crash and a fatal error (`runtime/main.c`
`crash_handler()`, `runtime/dispatch.c` `fatal_end()`) leave RTSS's profile to the next start through
`rtss_limit_crash`, which calls into RTSS's library only through the orderly restore and only when the state file
holds no line of the run. No GPU, no game files, no RTSS, any Python.

### `tests/rtss_limit_check.c`

The first tables are RTSS's own settings (which of them hold the game's frames, and the log's text of them) and the
kept-out reasons, for the log.

- The fake `RTSSSharedMemoryV2`: the header and application entries written as RTSS lays them out
  (`RTSS_SHARED_MEMORY_APP_ENTRY`: `dwFlags` after the 260-byte path). `parse()` checks `rtss_parse`: where the entry
  is, and that a bad header, a short entry or an array past the memory finds nothing; entries of the smallest size,
  ours the last that fits, are found, and past the memory's end nothing is read; with more than `RTSS_APPS_MAX`
  entries the walk stops there.
- The fake profile behind `RtssOps`: `refuse_above` (RTSS refuses a limit above this, in millihertz; 0 none), `holds`
  (the limit the profile holds from this run; -1 none). RTSS as the worker sees it: polls 500 ms apart; while it runs,
  lists the game and counts (`counting`) it measures a period of a second every second poll, with `frames` frames in
  it (the frame rate); the game presents (`presenting`) and the newest host present is a millisecond before the poll,
  else it stays where it was (0: none yet); `movie_mhz` is the rate a movie plays at (`rtss_limit_movie`). One poll
  for a want is made as `rtss_limit.c` `worker()` makes it: the hooks library loads on the first poll that wants it
  and stays.
- `injection_delay()`: RTSS runs but has not hooked the game for 30 polls. The limit goes into the profile at the
  second poll (the first sees the want new), with no second write. Once RTSS lists the game the game's limiter stands
  aside only after RTSS has seen its Vulkan presents (flags 0x7 is another API's value), read the profile and measured
  a period of its frames lately.
- RTSS quits while it limits, and comes back: the profile stays for its return. Profiles under Program Files without
  administrator rights: out for the run.
- `refused()`: a limit out of RTSS's range is refused and kept by the game; the earlier limit of this run is cleared
  to none; a limit in range goes to RTSS once it stays. Refused as the run's first write: the profile is the run's
  from then on.
- `frame_count()`: RTSS's own frame count. Each fresh period after a poll that stood aside is checked, and more than
  1.05 times the limit for three of them in a row puts the profile back for the run. 69 fps at 60 (1.15 times) is
  caught, 63 (1.05 times exactly) is not; a period RTSS no longer moves (the player lowered the limit and minimised
  the window) counts neither way; no limit and not listed never count. Two periods over, then one under: the count
  starts again. A want that changed while the poll ran: nothing decided, the count waits.
- `unproven()`: RTSS lists the game with the limit in its profile and pictures go out, but never shows its hold: it
  may hold the frames beside the game's limiter, so after `RTSS_UNPROVEN_POLLS` such polls in a row the profile goes
  back once, for the run. Each sign that never comes counts; a minimised window, RTSS not hooked and no limit do not
  count, and one poll that shows the hold starts the count again.
- `idle()`: no picture went out for 200 ms (a minimised window, a frame hold) or none yet: RTSS has nothing to hold,
  and the game's limiter takes the limit back until pictures go out again (a poll 199 ms after the last present is
  not yet idle). Before the first picture nothing is written.
- `kept_out()`: kept out and let in again: the first kept-out poll puts back what the run wrote, once; a limit
  afterwards is written at once and is the run's first write again. Kept out from the start: the hooks library is
  never wanted, and nothing is written or put back.
- `churn()`: low latency switching on and off at every poll (a swapchain rebuild, the time before the first
  swapchain): the want never stays two polls, so the profile keeps the limit written before, and the game's limiter
  holds the frames while the want is new. From the start, the same flips write nothing. A flip every two polls is a
  want that stays: each one acts.
- `movie()`: under RTSS the want stays the play limit and the game's pacer holds the movie. At a limit of 144 the
  profile keeps 144 through it, with no write; under the movie's rate (40) the profile has no limit for the movie's
  length, so RTSS does not hold the movie below its rate, and gets 40 back at its end; at the movie's own rate (60,
  the default limit) the profile keeps 60 through the movie, with no write: RTSS holds the movie beside the pacer, as
  `rtss_target` says.
- `unset_and_library()`: no want yet: nothing at all. The hooks library that does not load fails the run; while RTSS
  is not running the library is not wanted. (A run whose exit put the profile back makes no decision: `rtss_limit.c`
  `worker()` skips the poll, which the `restore_*` scenarios of `tests/rtss_worker_check.c` check.)
- Two limiters: RTSS out of the game's limit (kept out, or a profile that cannot be written or a hooks library that
  did not load), listed and a limit of RTSS's own; never for a limit the game hands RTSS.
- The period the game's pacer keeps beside RTSS: the ceiling while RTSS holds and host presents go out, the limit's
  own otherwise; never 0.
- `saved()`: the state file (`rtss-profile.txt`): its lines, the file's text as the worker rewrites it, and what a
  start does with a line that a run which did not close left behind. A line is found past a comment, a malformed line
  and another profile's line, whatever the case of its letters. Rewritten: ours replaced, the other kept, the
  malformed line and comments dropped, the header first. What a start does: run A made the profile and wrote 60/1, and
  was killed: B finds 60/1 and removes the profile. A run that wrote 60/1 and then 144/1 ended between the line and
  the profile's change, or after it: the profile holds 60/1 or 144/1, both the game's; anything else is the player's.
- The profile's values for a limit: whole rates as they are, a fraction over 1000 where RTSS knows the denominator,
  else rounded down, so "display" at 59.94 Hz never holds the game above the refresh.

### `tests/rtss_worker_check.c`

The real `runtime/pacing/rtss_limit.c` against a fake RivaTuner Statistics Server: the Win32 calls it makes go to the
fakes in the file (its folder in the registry, its hooks library with the profile calls, the profile files, the shared
memory `RTSSSharedMemoryV2`), and its worker thread polls when a scenario says so (`plat_sleep_ms()` on that thread
signals that a poll ended and waits for the next one to be started). Each scenario
(`rtss_worker_check <scenario>`) runs in a process of its own, since `rtss_limit.c`'s state lasts the process, with
the state file (`rtss-profile.txt`) in a folder of its own beside the program (`rtss_state\<scenario>`, emptied
first); the kill scenarios run two more processes of it (`--phase`), a run that is killed and the next start, which
share the fake profile through a file there. Windows only. It prints the log and each failed check.

The fakes:

- The profiles: Global, and the game's own `Profiles\ocarina.exe.cfg`. `LoadProfile` copies one into the loaded copy
  (Global for a name without its own), `SaveProfile` writes the loaded copy as the game's own. Each `[Framerate]` key
  `rtss_limit.c` reads is a field (`limit` and `denom` are `FramerateLimit` and `FramerateLimitDenominator`: `Limit`
  and `LimitDenominator` in the file); `stamp` is the file's write time, which every change moves on. Switches:
  `no_denominator` (an RTSS that does not know `FramerateLimitDenominator`), `readonly` (`SaveProfile` does not reach
  the file: no administrator rights), `refuse_over` (RTSS refuses a limit above this many frames a second; 0 none),
  `no_library` (`LoadLibraryA` fails), `calls` (every call into the fake hooks library), `during_save` (runs inside
  the next `SaveProfile`, once). `GetModuleFileNameA` is undefined again for `main()`: this program's own path, not
  the game's.
- The shared memory: the header and one application entry, this process's (listed) or another's. RTSS's clock and the
  game's presents as `rtss_limit.c` reads them: each `poll()` comes 500 ms after the one before, RTSS measures a new
  period of the game's frames at each (`counting`), and the game presents a millisecond before it unless a scenario
  stops that (`presenting`).
- The registry and file calls the thread that asks for a limit makes itself are counted: a present must make none.
- `host.h` and `platform.h`, as far as `rtss_limit.c` uses them: `quiet` (the stress scenario: no lines kept or
  printed); the state file next to the executable is the scenario's folder, with `state_readonly` (it cannot be
  written), `state_unreadable` (it is there but does not read: another program holds it) and `no_exe_path` (the
  executable's path is unknown: no state file at all); `state()` gives the state file as it is now, "" when there is
  none; `pause_at_lock` stops the worker's next `plat_mutex_lock` before it takes the lock; `free_run` lets the worker
  poll without waiting.
- The scenarios' steps: `poll()` is one more poll of the worker, waited for; `ask()` is a want at a present (the first
  one, which starts the worker, waits for its first poll, or for its end when it finds no RTSS); `settle()` is a new
  want, then the polls it takes to reach the profile (`RTSS_SETTLE_POLLS`).

The scenarios:

- The exit puts the profile back (`rtss_limit_restore`) and the game presents on until the process ends: a want after
  it (`frame_step_cap_ns` at a present after the restore) never reaches the profile, and the pacer paces again. The
  same with the player's own profile: it keeps the player's limit. A want the worker read before the restore (144000)
  and takes the lock with after it must not reach the profile. An exit before the first want, and one while RTSS was
  kept out: a want after it does not reach the profile.
- `NX2SARIA_RTSS=0` with a limit set in RTSS's Global profile: the worker only reads, never loads the hooks library,
  and tells of the two limiters until the player clears that limit.
- Low latency keeps RTSS out from the first present, with scanline sync set in the game's own profile (a negative line
  counts from the bottom); once low latency is off, a limit starts RTSS's part as usual.
- Frame generation keeps RTSS out, with no limit of RTSS's own: one limiter, and the line names frame generation.
  Switched on while the worker polls without pause (`active_race()`; with no state file, since 400000 writes and
  removals of it would take minutes): from the switch until the worker sees it, the game's limiter paces, never
  RTSS's hold of the limit before (the worker's store for a want that changed after it read it must not count for the
  new one). Switched on while the worker writes the first limit: RTSS holds that limit, but the game's limiter paces
  the kept-out want, so the log must not say RTSS does the limiting; the next poll logs the kept-out line. A later
  limit is held and logged as usual.
- A limit of a fractional monitor (display at 59.94 and 143.856 Hz): an RTSS that knows the denominator holds it as it
  is, one that does not holds it rounded down to whole frames, never above the refresh, and the log says what the
  profile holds, not what was asked.
- The first want comes at a present: the worker, not the present, reads RTSS's registry key and looks for its files.
  No RTSS: the worker says so and ends, the game's limiter paces, and the exit has nothing to put back.
- While RTSS lists the game, its settings for the game are logged once per change: Global's before the game has a
  profile of its own, then the game's own with the limit it wrote, frame time limit, scanline sync and the modes.
- The decisions as they stand, pinned against the real worker (`tests/rtss_limit_check.c` pins the same in
  `rtss_decide.h`). Profiles under Program Files without administrator rights: the save does not stick, and RTSS is
  out for the run; with a limit set in RTSS's Global profile RTSS still holds the game by its own setting, the log
  tells of the two limiters and the menu keeps its administrator note. A hooks library that does not load, with a
  limit in Global: the log tells of the two limiters; without one, the game limits for the run. RTSS refuses a limit
  out of its range: the game keeps that one, the earlier limit of this run is cleared to none, and a limit in range
  goes to RTSS again at once. RTSS lists the game but lets it run well over the limit: three fresh periods in a row
  over 1.05 times the limit, each after a poll that stood aside, put the profile back and the game limits for the run;
  1.05 times is not caught, a period RTSS no longer moves counts neither way while the game's limiter holds, 1.15
  times is caught. RTSS lists the game before it has seen a Vulkan present, and asks the game to read its profile
  again after each write: the game's limiter holds until both are done. RTSS never reads the profile again (its
  update flag stays set): after 30 s the profile goes back, the menu says RTSS does not limit the game, and the game's
  limiter holds for the run. No picture goes out (a minimised window): the game's limiter holds until pictures go out
  again, and the profile stays as it is. RTSS quits while it limits and comes back: the profile stays for its return,
  no write either way.
- Low latency switching on and off at every poll (the want flips between a limit and kept out, as at each swapchain
  rebuild): a want acts on the profile only once it stayed two polls, so the flips write and remove nothing, and the
  game's limiter holds each kept-out want at once. A want that stays two polls each time does act, and the log numbers
  each creation and removal so a play-test can count them.
- The state file: the run's first write records the profile as it was, each later one the value written, and the
  exit's restore removes the line and the file with it (`line_at_save` is the state file as `SaveProfile` found it).
  Kept out: put back during the run, and the line goes with it. A state file that cannot be written: the run goes on
  as before it, with a line in the log, and a crash puts the profile back as an orderly exit does. A put back during
  the run that does not land (the profile went read-only): the line and the values found at the run's first write
  stay; a later limit is the run's first write again and keeps the player's 30 to put back, not the game's 60 the
  profile still holds, and the exit puts 30 back. The same where the run made the profile: a removal that does not
  land is no removal; the line stays, and the exit removes the profile. A state file with another executable's line
  that does not read is left as it is, with that line, and the run goes on as with a file that cannot be written.
- A crash on RTSS's own thread, inside `SaveProfile` with the lock held, in a run whose state file could not be
  written: `rtss_limit_crash` closes the run and returns at once, with no call into RTSS's library and no wait for the
  lock that thread holds (`rtss_limit_restore` would try it for a second); nothing after it reaches the profile. A
  crash with the line in the state file: no call into RTSS's library, the profile and the line stay for the next
  start, and nothing after it reaches the profile.
- A run killed while RTSS held its limit (`kill_first()`), and the next start (`kill_next`): the profile as before the
  killed run, then the next run's own limit, and back again at its close. Variant 1: the player had a profile with
  30/1. Variant 2: the player set 75 in RTSS between the two runs. Variant 3: the next start has frame generation on,
  which keeps RTSS out, and still puts the profile back (the hooks library loaded for that alone) with no write of its
  own. Variant 4: the next start has `NX2SARIA_RTSS=0`, which keeps the thread read-only: the log says what the file
  holds, and the profile and the line stay for a start with RTSS let in. Variant 5: the run is killed inside the save
  of its second limit, after the line took 144 and before the profile did: the next start finds the 60 before it and
  removes the profile.
- A movie at 60 under a limit of 144: the want stays 144 (`runtime/pacing/frame_rate.c` `frame_step_cap_ns()`) and
  RTSS's profile keeps it with no write, while the game's pacer holds the movie at its rate; RTSS holds the game again
  at its end. Under a limit of 40 the profile has no limit while the movie plays, so RTSS cannot space out the frames
  the pacer sends at once after a late one, and gets 40 back at its end.

## `tests/rumble_hd_check.py`

HD rumble for controllers with linear actuators (`runtime/input/rumble_hd.c`): builds it with
`tests/rumble_hd_check.c` and runs it; no game files, no SDL, no controller
(`python tests/rumble_hd_check.py [--cc <clang>]`; the `--help` text is the string `DESCRIPTION`). The C program
checks a vibration value as the bytes of a linear actuator's rumble report, a report's data from the two sides, and
which reports are written when the game sends a batch; with `--sweep` it prints values and their bytes, one per line,
for the script to decode with formulae of its own.

- Known values. The neutral value, as the game sends it and as the controller expects it. Both bands at 150 Hz and
  amplitude 0.9: frequency step 125 (low 0x3d, high 0x74), amplitude step 95. Full amplitude at the neutral
  frequencies: amplitude step 100, the last one. Amplitude 0.5 is step 68; one band alone leaves the other at step 0.
  An odd amplitude step puts its low bit on top of the low frequency byte: 0.245 is step 35.
- The clamps. Frequencies outside a band go to its ends: low 1..0x7f, high 4..0x1fc. Amplitudes above 1, by themselves
  or through the strength, stop at step 100; the strength scales below it. No amplitude is the neutral value whatever
  the frequencies say: negative, NaN, too small, strength 0.
- The amplitude scale.
- A report's data from the two sides, for two actuators and for one: one actuator takes the stronger side (the right
  one, 0.9) in both halves, and the side that is not at rest when the stronger one is. The output report's layout.
- From a batch to the reports written, with a tick every 5 ms and 15 ms between reports. Three values in one batch:
  the newest is written, at once. A value that comes 5 ms after the first report is too soon to write, and one that
  replaces it before it was written is the one written (at 16 ms); it is stale 250 ms later, and written every 30 ms
  until then. Once the game stops the neutral value is written three times, then nothing. The path turned off while
  the game sends: the neutral value, whatever the game's data; back on with the game's data still fresh: it is written
  again. Another controller: what was written is forgotten, the game's value is due at once.

The Python side then decodes the sweep with formulae of its own (the script's, not the C file's): each frequency comes
back within half a step of the clamped input, each amplitude as the step nearest to it, and no amplitude step is above
100. `LOW` (40.875885, 626.286133) and `HIGH` (81.75177, 1252.572266) are the ends of the low and the high band in
Hz. `step_amp()` is the amplitude of step k, from the published table's rule: step 1 alone, then quarter, sixteenth
and thirty-second octaves. `decode()` turns four report bytes into (amp_low step, freq_low Hz, amp_high step,
freq_high Hz). In `check_sweep()` the tolerance is half a frequency step (2^(1/64)) and the rounding of a float
(1.0005), and at a boundary between two steps either neighbour is the nearest.

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

`NOT_SOURCES` (`platform`, `shaders`, `third_party`, `dlssg`) are the folders compiled apart, or not part of the game's
compile.

## `tests/runtime_include.py`

Not a test: the include flags of the runtime's sources for the tests that compile them, `runtime/` and its folders by
subsystem (`tools/build_game.py` `RUNTIME_DIRS`), as the build puts them on the path, since the sources include one
another's headers by name. `flags()` returns `['-I', <rt>, '-I', <rt>/audio, ...]`, where `rt` is a `runtime/` folder,
this checkout's by default.

## `tests/save_retry_check.py`

Builds and runs `tests/save_retry_check.c` on `runtime/vk/vk_save_retry.h`: when the autosave thread saves the
pipeline caches and when it tries again after a save that did not happen (no copy of the data, a file that cannot be
written; `runtime/vk/vk_pipeline.c` `autosave_once()`). Without the differential tests' packages it makes the same
compiler choice without them. Times are ms since the machine started, as the thread's clock gives them.

- Nothing built, nothing owed: no save, however long the run.
- A new pipeline: a save, and after it none until the next one; never two within the interval.
- A save that did not happen is tried again at the interval with nothing new built, `SAVE_RETRIES` times; the last of
  them says so, and then nothing is tried until a new pipeline. A retry that saves ends the row: nothing more until
  something new.
- A save owed from the start (the play file folded in) is tried with nothing built, and bounded the same way.

## `tests/save_share_check.py`

Builds and runs `tests/save_share_check.c`: the game's side of a shared save without the game, `runtime/hle/fs_host.c`,
`runtime/hle/save_share.c` and `runtime/hle/save_zip.c` with the rest of the runtime stubbed. Each run of the program
is one start of the game on a profile folder the test makes: the first storage use (`fs_save_export`) runs
`storage_init`, which makes a waiting import the save (`fs_host.c` `import_pending`). Python 3.12 or later; no game
files.

The program's modes: `export PROFILE DIR [bug]` (`save_share_export` into DIR; prints "result N path P"),
`import PROFILE ZIP` (`save_share_import`; prints "result N detail D waiting W"), `start PROFILE` (a start and nothing
else; prints "done D waiting W"), `cancel PROFILE` (`save_share_cancel`; prints "result N waiting W"). Backups go to
`saves/` beside the program (`save_share_backup_dir`), as the game's go beside the game.

The stubs are for the runtime around `fs_host.c` (`host.h` and the files that define these in the game); only what the
save paths call does anything, and `fs.h`'s own declarations of some would clash with stubs that only have to link.
`rt_config` returns NULL (`fs_host_set_profile` names the profile first). RomFS, the host function registry, guest
memory and the probes are never reached on the save paths (`perf_size` is `fs_host_sizes`': only the perf trace's size
lines call it). `mem_probing` answers 0: `write_file` asks before each write, and there is no probe and no lazy heap
here. No boot map is pending (`runtime/patches/boot_map.c`), so a save file is read as it is; `log_short_paths` is off
(`runtime/log_privacy.c`), so the text is shown as it is. `ACCOUNT` in the Python file is the C file's `account_uid`,
as `fs_host.c` names it.

- Export: nothing saved yet is refused; then the zip holds the save the game would load next (a finished commit
  waiting in `save-tx` before the committed save; the start moved the finished commit in, `swap_in`, before the export
  read it), its manifest as the launcher writes it (`launcher/saves.py` `check_manifest()` takes it), no key file (a
  stray `prod.keys` in a save folder never goes into a zip); with a bug report the log and `nx2saria.ini` (reading an
  existing `nx2navi.ini`).
- Import: zips the launcher exports, from another game folder, wait in `profile/save-import`; at the next start the
  save the game would load goes to `saves/backup <time>` beside the program, the import becomes the save, and the
  start says where the old save went; a second start changes nothing. A finished commit made after the import (the
  game saved before the restart) is backed up and replaced too.
- Refusals (no manifest, another game, a newer format, a path outside the save, a damaged member, no save files):
  nothing waits and the save stays; Cancel drops a waiting import.

## `tests/save_zip_check.py`

Builds and runs `tests/save_zip_check.c`: `runtime/hle/save_zip.c`, the zip of a shared save as the game's menu writes
and reads it (`runtime/hle/save_share.c`), against Python's zlib and zipfile, which the launcher uses for the same zip
(`launcher/saves.py`). No game files. The program's modes:

- `selftest`: the manifest's JSON keys, round trips in memory, damaged and cut input. The codec case uses text with
  repeats, then bytes of every value: literals of both fixed code lengths, long and far matches. Damaged data is
  refused, by inflate or by the CRC; for a stored member's byte only the CRC tells; every prefix of the zip is
  refused, never read past its end.
- `inflate IN SIZE OUT`: raw deflate IN (zlib's) into SIZE bytes at OUT; exit 3 when refused. The streams zlib makes at
  every level and strategy (stored, fixed and dynamic blocks) inflate to the same bytes, and a damaged one (a flipped
  bit in a dynamic block, and one cut short) is refused.
- `deflate IN OUT`: raw deflate of IN into OUT, which zlib inflates to the same bytes.
- `zip OUT NAME FILE [NAME FILE]`: a zip of the files under those names; it passes zipfile's `testzip` with the same
  members.
- `unzip ZIP DIR`: every member into `DIR/<index>`, a line "name|crc|size|dir|link" each; exit 3 with the `SZ_E_*`
  code when a member or the zip is refused. Zips zipfile makes (stored, deflated, with data descriptors, the
  launcher's export) unzip in C to the same members with their CRCs; a link member, as a Unix zip tool stores one, is
  marked; a damaged member is refused by CRC or inflate. `NoSeek` is a write-only stream zipfile cannot seek in: it
  writes data descriptors after the members.

Last, the launcher imports a zip the C writer made (`launcher/saves.py` `import_zip()`).

## `tests/scratch_lru_check.py`

Builds and runs `tests/scratch_lru_check.c` on `runtime/vk/vk_scratch_lru.h`: which entry of the upscaler stand-in's
scratch image cache serves a request (`runtime/vk/vk_exec_draw.c` `fallback_scratch()`): a free one, else the one used
longest ago. The old cache kept its first eight sizes and refused the ninth: the sizes in the check are those a run
walks through when the window and the DLSS mode change a few times. No GPU, no game files, any Python.

The C file's request helper works as `fallback_scratch()` does: a hit returns its entry, a miss fills the entry it was
given (after freeing the evicted image); it returns the entry, and the hit and eviction as `scr_pick` said.

- Empty: free entries in order, nothing evicted.
- A match is a hit and refreshes its frame. Same size, another format: not a match.
- The ninth size: the one used longest ago goes (entry 0, frame 1), not the full cache refusing it. The refreshed entry
  3 (frame 100) outlives the untouched ones.
- Two images in one frame never evict each other while older entries remain (g8 then g8s of one dispatch).
- A freed entry (an image that failed to be made stays not live) is taken before any eviction.

## `tests/sdl3_pin_test.py`

`tools/sdl3_pin.py`, the SDL3 a Linux build kit carries for systems that have none (issue #11: Ubuntu 24.04 and Linux
Mint 22 package no SDL3). No network, no game files: the packages and the sysroot are made in the test (`ar()` writes
an ar archive, a Debian package, of (name, bytes) members; `tar_bytes()` a tar archive of {name: bytes} regular files,
(name, target) symbolic links and (name, target) hard links). The sysroot holds the same files where the packages
install them, beside others, the copyright file a hard link to the development package's.
`Sdl3PinTest.resolve()` runs `sdl3_pin.resolve` quietly and returns (folder or None, the message it stopped with,
what it printed); `Sdl3PinTest.urlopen()` serves {url: bytes} and answers 404 for any other address.

- The pinned packages give the headers, the library and its copyright file and nothing else: the library's file
  itself under its soname, not the package's link. A folder there already is not fetched or unpacked again; one cut
  short while it was written (no stamp), or with a stamp that is not ASCII, is.
- Another package, a package without what it should hold or with other files is refused.
- When the packages are gone from the pool the same files come from the dated sysroot, checked against its SHA-256:
  the first 404 ends the pool's turn; the library taken is the 64-bit one. The same archive from disk works; another
  archive is refused, and a sysroot that cannot be fetched says what to do. A download cut short after the archive's
  end marker is told from another file (the pinned file is 100 bytes longer than the local one).
- Only what an earlier unpack wrote is replaced, in a folder that holds other things too.
- A damaged package is an error with a reason, not a crash; members of odd sizes are padded.

## `tests/session_summary_check.py`

Builds and runs `tests/session_summary_check.c`: `runtime/session_summary_fmt.c`, the JSON text of
`logs/session-summary.json` (`runtime/session_summary.c` writes it at the game's end). The C side writes each case's
JSON to `<dir>/<case>.json`; the Python side parses it. Each sample summary (all fields, all null, hostile strings and
numbers past the limits, a device-lost end) must parse as JSON with exactly the summary's keys, each value of its type
and within its limits: strings printable and capped, settings and display values in `[A-Za-z0-9_.:x+-]` or "other",
vendor and device as 0x and 4 hex digits, numbers with at most one decimal. It also checks statically that the two
ends call the writer where they should (`runtime/gfx/gfx_present.c` `rt_exit_steps()`, `runtime/dispatch.c`
`fatal_end()`) and that the hard crash (`runtime/main.c` `crash_handler()`) writes none. No game files.

In the Python file the schema table holds the summary's keys and the types of their leaves: str, int, float (an int or
a number with one decimal), bool, 'hex' (0x and 4 hex digits) and 'token' (a display or settings value); None in a
tuple means the leaf may be null. `check_leaf()` returns the problems with one leaf, as a list of strings;
`check_obj()` never accepts a null group: the server refuses one (`telemetry/src/validate.ts`), its leaves are null
instead. `expect()` is what each case must hold beyond the schema; the names that are not a patch set's (spaces,
empty; a mod's set is still a name) are left out. The files are read as strict UTF-8, so invalid UTF-8 fails there.

In the C file:

- Hostile strings: quotes, backslashes, line ends and other controls, invalid UTF-8, non-ASCII text, a C1 control, a
  surrogate's encoding, over-long names and fatal lines, tokens outside the charset, and numbers past the limits.
- A fatal line from a path in a home folder, already short (`log_privacy_str` runs before), and the device-lost end.
- `ss_format` refuses a buffer too small, and writes a terminated string anyway.
- `ss_setting`: the list's spelling, ranges, "other", free text, NULL. `ss_os_name`.
- `ss_frametime`: `perf.c`'s 0.25 ms bins. 1000 frames: 985 at 16.6 ms (bin 66), 10 at 33 ms (bin 132), 5 at 120 ms
  (bin 480). The slowest 1% is 10 frames: the 5 of bin 480 and 5 of bin 132, so the 1% low is 1e9 / 33.25 ms. Under
  100 frames the slowest one alone is the 1%; a frame past the last bin lands in it.

## `tests/sha256_paths_check.py`

Builds and runs `tests/sha256_paths_check.c` with every clang this machine has: `runtime/sha256.c`'s portable C and
SHA-NI block functions give the FIPS 180-4 hashes and the same hash of every message, and its CPUID check finds the
SHA extensions exactly where the processor reports them (`/proc/cpuinfo`'s `sha_ni` on Linux;
`__builtin_cpu_supports` where the compiler takes "sha", clang 19 and later). The compilers are the one the build
uses (`CC`, MSYS2 UCRT64's clang on Windows, else clang on `PATH`) and, on Linux, each `/usr/lib/llvm-*/bin/clang`,
so that a distribution's older default (Ubuntu 24.04's clang 18) is covered beside a newer one; each builds the file
generic (as `tools/make_game.py` builds the runtime), for x86-64-v2 (`tools/build_game.py --cpu v2`) and, where this
CPU runs it, x86-64-v3. No game files. `compilers()` returns (label, path) of each distinct clang to try, the build's
own first; a `CC` that does not resolve fails; on Windows only a MinGW clang builds the runtime (an MSVC-targeting
one lacks its headers here). `cpu_flags()` gives the processor's feature flags from `/proc/cpuinfo`, None where there
is none to read.

The C program runs the FIPS 180-4 vectors (the examples, the 448-bit two-block one, a million 'a') and HMAC-SHA256
test case 2 of RFC 4231 through the public calls, once in portable C and once on the SHA extensions when the CPU has
them, and both forms against each other: whole messages of every length from 0 to 300 bytes and some long ones, each
in one call and in pieces that cut the 64-byte blocks in different places, and the two block functions on the same
state and blocks, without the padding the public calls add. Every SHA-NI round instruction the hashes run is counted
(`rounds`: the `sha256rnds2` instructions `blocks_ni` has run), so a run that was meant for one form and ran the other
fails instead of comparing portable C with itself: `rounds` grows with SHA-NI and stays put in portable C. It prints
"cpuid sha_ni=<0|1>" (what `sha256_ni` found), on clang 19 and later and GCC also "builtin sha_ni=<0|1>" (a second
opinion), then "compared ni=<0|1> ..." (whether the SHA-NI form ran) and one line per failure.

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

## `tests/skin_pair_check.py`

Builds and runs `tests/skin_pair_check.c` on `runtime/vk/skin_pair.h`: which draw of last frame a skinned draw
continues, for the motion vectors of skinned characters. No GPU, no game files, any Python (`compiler()` as
`tests/fg_reset_check.py` picks it).

A character of the scene (`Actor` in the C file) is its mesh and program (the key), its place in the frame's command
stream, and the two copies of its palette block, which the game writes in turn. The frame helper draws the actors as
frame number f (reached by the replay's threads in the order of `reached`, NULL: as given), pairs them with the frame
before when `consecutive`, and returns the frame's entries; the entry of an actor is found by tag, and another helper
gives the actor the paired entry of last frame belongs to, -1 when the entry is not paired.

- One character whose palette copies take turns: nothing in its first frame, its place's address is not known in the
  second, paired from the third on. A frame without a list in between: last frame's list is not of the frame before;
  its two addresses are still known, so the frame after the gap is paired.
- Written every second frame only: the copy it stays on and the one it moves to are both its own. A block the game
  does not write again keeps its address: paired in the second frame.
- Two characters of one model: the same key twice in a frame. The numbers follow the command stream, whatever order
  the replay's threads reached the draws in. They change places in the draw order: each place now holds the other's
  palette, at an address the place never had, for two frames; then both are paired with themselves again.
- A third one of the model appears: every draw of the key is in doubt that frame, and the old two are paired again
  once the count has held for a frame. One of them leaves. The last one leaves in the frame another one of the model
  comes: one draw as before, but not the same character.
- The palette changes size. A key that was not drawn, and one in an empty list.
- `skin_near`: every bone's translation within the limit; a turned bone at the same place.

## `tests/spin_waits_test.py`

`tools/spin_waits.py` on hand-assembled AArch64 loops, without game files (`python tests/spin_waits_test.py`). The
main sample is a job queue poll: take the queue's spin lock, look whether the wait is over, release the lock, again.

```
0x00 T:  cbz x20, X                 0x2c     str w12, [x20, #0x100]     0x50     ldr w13, [x21, #0x90]
0x04     str w11, [x20, #0x100]     0x30     ldrb w14, [x21, #0x98]     0x54     sub w8, w13, w25
0x08 L0: ldr w14, [x9]              0x34     cmp w0, #0                 0x58     add w15, w8, w25
0x0c     cmp w14, #1                0x38     ccmp w14, #0, #4, ne       0x5c     str w15, [x21, #0x94]
0x10     b.eq L0                    0x3c     ldr w25, [x21, #0x94]      0x60     cset w13, hs
0x14 L1: ldaxr w14, [x9]            0x40     ccmp w25, #0, #0, ne       0x64     b U
0x18     cbz w14, L2                0x44     cset w14, gt               0x68 C:  clrex
0x1c     clrex                      0x48     cmp w14, #1                0x6c U:  ldxr w15, [x9] / cmp w15, #1 / b.ne C
0x20     b L1                       0x4c     b.ne U                     0x78     stlxr w15, wzr, [x9] / cbnz w15, U
0x24 L2: stxr w14, w10, [x9] / cbnz w14, L1                             0x80     cbz w14, T          0x84 X: ret
```

0x50-0x64: the wait is over, take an item, store the new cursor, release and leave.

- The lock poll is a wait; the path that takes an item stores a new cursor (0x5c), so it is not part of the wait.
- A site of the patch table in the region keeps it out.
- Not waits: the wait path stores w14, which the pass writes (a store of a changing value); the wait path skips the
  release (`b.ne U` turned into `b.ne` to the back-edge: the lock is not released); `str xzr, [x8]; b back` (stores of
  a constant and no load: nothing polled); `ldr w8, [x0]; cbz w8, back` (a plain test loop);
  `ldr w8, [x0]; subs w1, w1, #1; b.ne back` (a counter: it leaves by itself).

## `tests/sprint_check.py`

Builds and runs `tests/sprint_check.c`: the sprint button (`runtime/patches/sprint.c`) against a stand-in Link, with
the rest of the runtime stubbed. No game files: the module layout and patch page the runtime includes (`layout.h`,
`patches.h`) are stand-ins written to the test's build folder. The C file checks the presses and the button the game
gets from each controller poll (`sprint_pad`), and the dash input the patch set's two host functions put into Link's
flags, frame by frame, for toggle and hold, each button, Link's states and a Link the player does not control.

The stand-ins: `test_main_base` is `layout.h`'s `MOD_MAIN_BASE`, a buffer standing in for main's image; `set_on` is the
patch set `sprint`, on at the start as its default; `step` is `frame_step_frames`, the 60 Hz frames of one update of
Link; `log_lines` counts the lines `sprint.c` writes through `rt_log` (a press an event takes is named). Link is a
vtable, +0x18 -> owner whose +0x270 -> a with +0x188 -> b with +0x1c0 -> the state, id at +0x28. `frame()` is one
frame: a controller poll with the given buttons, then Link's update (other flag bits, 0x40, stay; the sites call in
only while the set is on); it returns what the game got from the poll. `reset()` runs 12 cutscene frames: the settings
change and every sprint ends.

- Off at the start (`NX2SARIA_SPRINT_BUTTON_PATCH=0`, or a mod patching its sites): no rows, never switched on, and the
  button always reaches the game.
- The actor manager names the controlled Link (the one whose +0x18 is owner); another object of Link's class
  (main+0x4b4dfc0) has another owner.
- Off: nothing changes, the button reaches the game, and the set's sites stop calling in. A button chosen: the set
  runs again.
- Toggle on L3: a press while walking starts it; the button never reaches the game during play. Held is no second
  press; it stays on through a jump and a roll and after it; the next press ends it. Standing still ends it after 4
  frames once it has moved; a press while standing waits up to 90 frames for Link to move. An attack, targeting or a
  cutscene ends it at once (ZL reaches the game and targets); a press during an attack does not start it.
- Link's class without the player's control: no dash input, and the controlled Link keeps his sprint.
- A menu or a load: Link's update stops running, and after 8 polls the button reaches the game again; the next update
  takes the press made meanwhile.
- Toggle on the bottom button: a press while not moving (put the weapon away) reaches the game and starts nothing;
  once Link moves, from the next poll the button is the sprint's; not moving, the button keeps its own job.
- Hold on L3: sprint while held; L3 never reaches the game during play, other buttons do. A button the setting no
  longer offers is off.
- Hold on the bottom button: starts only while moving, keeps going while held. A press begun while standing stays the
  game's for its whole length, also once Link moves: no sprint, no strip. A press begun while moving stays the
  sprint's when Link stops: the game gets no B from it.
- A press made in a menu (no update of Link for more than 8 polls) starts nothing when play goes on: it was the menu's.
- A conversation: Link's update goes on, but an event runs (the word at +0x68 of the event object), so L3 (hide the
  text box) is the game's and starts nothing; the log names the press ("while an event runs"); when the event ends the
  button is the sprint's again.
- The stand-still timings count 60 Hz frames: at 240 fps (a quarter frame an update) a stop of four updates goes on.
- Switched off with the sprint on, and on again: it does not come back by itself.

## `tests/spv_store_check.py`

Builds and runs `tests/spv_store_check.c` against the platform layer (with `stand_ins()` of
`tests/platform_stand_ins.py`) and libzstd: `runtime/gfx/spv_store.h`, which keeps a shader's SPIR-V zstd-compressed
while nobody reads it (`runtime/gfx/gfx_shader.c`, "SPIR-V at rest"). No GPU, no game files, any Python.

- The single-thread checks walk one store through its states: plain, pinned, packed (the frame made, checked and the
  plain copy freed), decoded back by a pin bit for bit, idle again (only the plain copy goes, the frame stays), and its
  FNV-1a read from a compressed store without a decode. A reader that arrives while the packer encodes keeps the plain
  copy.
- The stress check runs reader threads that pin random stores and compare every word against the original (holding
  some pins across a time slice), while a packer thread packs every store as soon as nobody holds it; no reader may
  ever see other words, every pin must be given back, and the final state still gives back the original.
- The test data are SPIR-V-like words: the header, then instructions with small opcodes, word counts and ids. They are
  more random than the translator's modules (a frame keeps about 65% of them, against 26% of real SPIR-V), which only
  makes the encoder's work larger.

## `tests/stutter_hitch_check.py`

Builds and runs `tests/stutter_hitch_check.c`: `runtime/diag/stutter_map.c`'s hitch rule on made-up frame times, with
the few runtime functions it calls stubbed. A steady change of pace (the game drawing at 60 fps after 200) counts no
hitch; isolated long frames each count one; a frame over `SM_HITCH_SURE_NS` counts whatever follows it; frames right
after an area load count as the load's. No GPU, no game files, any Python. `runtime/simd_fp.h` `FOK` ors two compares
on purpose, and clang 18 warns on it.

The C file keeps the last report's first line (`head`), lists `perf.c`'s causes in its order (`FT_C_*`; the frames are
charged to the last, unattributed), and reads the hitches in play the exit's report prints and the ones the areas hold
(what a report counts so far). The clock starts well past any area load.

- 300 frames of the game's menus at 60 fps: none. Five isolated spikes of 30 ms among 7 ms frames: five.
- Two long frames in a row (150 ms each): one episode of 300 ms. A double spike of 40 ms: one episode.
- Three long frames, each followed by one as long, as after an area load in a session of 2026-11-13 (87, 91 and 96
  ms): one episode, not three changes of pace.
- A change to 30 fps from 140: a run of long frames longer than `SM_EPISODE_MAX`, a change of pace.
- `stutter_area("Kakariko")`, a load: what follows within `SM_LOAD_NS` is the load's. `stutter_load()` is the texture
  pool's load of an area, without the game's.
- At 30 fps every frame is over `SM_HITCH_ANY_NS` and so a candidate: a hitch followed by the steady frames ends at
  itself, not at the end of the slow run (an episode of 301 frames, 10 s long, before). The end of the slow run stands
  out from the frames after it: one more. A steady 30 fps with a 120 ms hitch every 60 frames: each counts.
- The exit's report counts a hitch in the last frames, which only the next frames would have decided: a frame over the
  sure line followed by 0, 1 or 2 frames (a waiting candidate, then an open episode), two such frames in a row (one
  episode) and three long frames ending the run (one episode); each time what the run counts once it goes on.

## `tests/stutter_map_check.py`

`tools/stutter_map.py` reads the lines `runtime/diag/stutter_map.c` writes: lines made with the runtime's format
strings parse into the areas, causes and pipelines they hold. The format strings are copied into the test and must be
kept in step with `stutter_frame` and `stutter_report` in `stutter_map.c`; the test first checks that the ones the
parser depends on are still in the runtime. `c_fmt()` turns a C printf format with `%llu`, `%u` and `%d` into
Python's `%d`. The samples include an episode of three frames, whose head carries their summed time, and a cause with
a hyphen in its name.

## `tests/submit_clone_check.py`

Builds and runs `tests/submit_clone_check.c` (with `stand_ins()` of `tests/platform_stand_ins.py`):
`runtime/nvn/nvn_submit_guard.h`, a synchronous submission's deferred binds when another guest thread may be replaying
the same recording copy (`runtime/nvn/nvn_api_cmd.c` `nvn_submit_prepare()`, `nvn_submit_done()`). No GPU, no game
files, any Python.

- Four threads submit two hand-built copies 200,000 times each, a quarter of the calls giving their copy twice, while a
  "game" thread rewrites the handle arrays their deferred binds read. A replay must see its copy's handles unchanged
  from its resolve to its end (it reads the binds at its start and at its end, where a held continuation would copy
  them), the in-flight count must be back to 0, the in-place, the given-again and the clone path must all run, and a
  copy given twice in one call must stay in place when its first occurrence does. The submitter follows
  `nvn_submit_prepare()`, the replay, `nvn_submit_done()`; handles are copy numbers + 1, a clone's `out` is 0.
- `nvn_copy_again` with `nvn_copy_enter` as `nvn_submit_prepare()` uses them, one thread: occurrence i of handles h
  stays in place (1) or is cloned (0); a clone's `out` is 0 and leaves its count at once. A copy given twice in one
  call with nothing else in flight: both in place, two counts, both left after the replay. Another thread's submission
  in flight: both occurrences cloned, the count back to that one's.
- `nvn_resolve_binds`, one thread: an array whose handles differ from the game's is read and written (copied in and
  counted); one made equal is read, not counted as changed; an immediate bind (no `src`), one with n 0 and one with n
  over 32 stay untouched; a bind reached through `OP_CALL` is resolved; `OP_CALL` is followed 16 deep and the 17th call
  stops it; a corrupt record (a size not a multiple of 8) is reported where it is.
- The argument `old` resolves in place without the guard (0.10.1) and only prints what it sees, to show that the
  harness catches the race.

## `tests/surface_formats_check.py`

Checks the runtime's surface-format enumeration with mocked Vulkan queries, without game files or a GPU
(`python tests/surface_formats_check.py [--sanitize]`; `CC` selects a C compiler, otherwise clang, or MSYS2 UCRT64's
clang on Windows; the `--help` text is the string `DESCRIPTION`). It compiles the actual helper, not a duplicate of
its implementation: the rest of the presenter needs a window, a Vulkan device and generated game headers, none of
which this API-enumeration regression needs. In `tests/surface_formats_check.c` the mocked
`vkGetPhysicalDeviceSurfaceFormatsKHR()` puts a usable HDR10 pair beyond the former 64-entry cutoff.

## `tests/sync_hw_check.py`

Builds and runs `tests/sync_hw_check.c` on `runtime/vk/vk_sync_hw.h`: the newest value each of the replay's timelines
may reach, and the test that takes a value read above it as a lost device (`runtime/vk/vk_sync.c`, "a lost device in
the timelines' values"). No GPU, no game files, any Python.

- A note only rises.
- What a live device can read, and what it cannot: with submissions 1..11 noted, the timeline at any of them, or not
  yet at the first, is a live reading. After a timeout reset Windows releases the waiters of a timeline with the
  largest value. A note at the largest value (nothing above it can be read) never counts a reading as lost.
- The restart of `vk_sync_start`: the note is raised to the value signalled before the signal, so the completion
  thread's first reading after it is a live one (a new device: its timeline at 3, the front end's values at 500).
- Four threads raise one note with interleaved values, as the submitting thread, the present and the probes may:
  concurrent raises end at the largest value any thread gave.

## `tests/sync_wake_check.py`

Builds and runs `tests/sync_wake_check.c`: which WaitSynchronization waits a signalled event or an ended thread wakes
(`runtime/hle/kernel.c` `wake_sync_waiters()`), on guest threads whose entries are host functions, with the platform
layer linked (and `stand_ins()` of `tests/platform_stand_ins.py`) and the rest of the runtime stubbed. No game files:
the module layout the runtime includes is a stand-in written to the test's build folder. With the argument `all` the
program runs with `NX2SARIA_SYNC_WAKE_ALL=1` and checks that every wait is woken and blocks again.

The stubs for what `kernel.c` reaches: `mem_readable` answers 0 (`NX2SARIA_SVC_WAITS` walks guest frames: off here);
`cfg_value` answers "0" (no asynchronous preemption, no slow-service log); `perf_size` does nothing (`kern_sizes` is
not called); `mem_commit_checked` of `runtime/memory.c` is there because `kernel.c` commits the main thread's stack
with it, which these tests never start. A guest entry is a host function taking the thread's Cpu, with its argument
in x0. A waiter has `loop` (clear the event, SVC 0x12 ClearEvent on the readable end, and wait again after each wake,
counting in `wakes`) and `hold` (1: the thread does not end after its wait until the test sets it to 0). A thread that
has ended: `kernel.c` `thread_finish` marks it exited and wakes its waiters in one hold of the kernel lock. The count
of waits blocked now tells that those woken for nothing are back in their wait.

1. A waits on a, B on b, C on b and c. A signal of a wakes A; B and C stay in their waits. A's thread ends only once B
   and C are back in their waits (with `NX2SARIA_SYNC_WAKE_ALL` the signal woke them too), so that its end finds them
   there; the counts are taken after it ended and they blocked again. Two wakes pass B and C by: the signal of a, and
   the end of A's thread.
2. A wait on several handles is woken by its second one and reports its index.
3. A thread that ends wakes the wait on its handle: E waits for B's thread, and b ends B.
4. A timed wait keeps its time across other events' signals; a wait without handles is a sleep.
5. CancelSynchronization ends a wait whatever it waits on.
6. No signal is lost: eight threads each wait on their own event in a loop; every signal is seen by its thread. To end
   them, the last wake is not followed by another wait.

## `tests/system_event_check.py`

Builds and runs `tests/system_event_check.c`: the system-event table of `runtime/hle/kernel.c`
(`kern_bind_system_event`, `kern_signal_system_event`, `kern_clear_system_event`, `kern_unbind_system_event`), with
the platform layer linked (and `stand_ins()` of `tests/platform_stand_ins.py`) and the rest of the runtime stubbed. No
game files: the module layout the runtime includes is a stand-in written to the test's build folder.

Without an argument the program checks:

- A kept handle the guest closed (DestroySystemEvent) is neither signalled nor cleared, also when its number went to
  an unrelated event or the state byte reads nonzero again. With state byte 0 the signal is skipped before the kernel
  lock. When the byte reads nonzero again with the handles closed (the memory reused, or the SDK made the event again
  without the binder), the signal must neither end the process nor reach anything. The closed number goes to the next
  new event at once: the stale entry must not set or clear it. Bound again after the destroy (state byte 0 first, as
  the guest leaves it), it signals again.
- A host thread signalling in a loop while a guest thread destroys the event and binds it again (the override called
  again after a DestroySystemEvent), as `runtime/gfx/gfx_present.c` `vsync_main()` does with a vsync event, never ends
  the process.
- With the table full, a new bind takes a destroyed guest-memory entry and keeps the entries in host memory, which a
  host thread then still signals (`runtime/hle/ov_movie2.c` `update_events()` on a movie decoder thread). The case
  fills the rest of the table with live events and the movie's two, which live in host memory, and makes the two
  destroyed guest entries again, so every entry is live.
- `kern_system_event_live` tells live guest events from destroyed ones.
- `kern_unbind_system_event` destroys a host-memory event and frees its entry, as `runtime/hle/ov_movie2.c`
  `ov_dtor()` does with the table full of live guest events and the movie's two host-memory events: each unbound event
  is destroyed (its handles closed) and leaves the table, the other keeps its entry, a second unbind finds nothing and
  leaves the table alone, and once the last host entry is gone a new guest bind finds room (the full-table scan keeps
  host entries, so it would otherwise end with "table full").

With the argument `full` every entry is live and the next bind ends the process with "table full"; the script checks
the exit code (`rt_fatal`'s stub exits 3) and the text.

The stubs: guest memory is one block; anything else (the "host-memory" events) is not, as `memory.c` answers. The
SDK's functions the table calls are 1 CreateSystemEvent, 2 GetWritableHandleOfSystemEvent (`kern_bind_system_event`)
and 3 DestroySystemEvent (`kern_unbind_system_event`). CreateSystemEvent(ev, mode, ipc) makes a kernel event, keeps
its writable handle at +0 of the `SystemEventType` and its readable one at +4, and sets the state byte;
GetWritableHandleOfSystemEvent(ev) reads +0; `nn::os::DestroySystemEvent` clears the state byte first, then closes both
handles (SVC 0x16). The main thread plays a guest thread for the binds. `perf_size` and `mem_commit_checked` are
stubbed as in `tests/sync_wake_check.c`.

## `tests/target_frames_check.py`

Builds and runs `tests/target_frames_check.c` on `runtime/vk/vk_target_frames.h`: which render passes count as
rendered frame after frame, so that a draw into them is skipped while its pipeline builds instead of holding the
replay thread. No GPU, no game files, any Python. The C file's verdict helper gives the verdict with the steady rule
and without it, and whether only the steady rule made it per-frame.

- The area change of a player's log: an area change after 238 frames of loading screen, where every pass read as
  one-shot and six mesh draws waited 52 to 660 ms each. The targets: the scene and its depth (drawn every frame), a
  probe and its depth (every second frame), a layer of the probe drawn every 15th frame, the half-resolution buffers.
  The frame after, a pass is per-frame by being recent, with or without the rule. 238 frames of loading screen later
  (its own targets are other images) only the steady rule makes them per-frame. The 960x540 pass of the log: one colour
  target made new, one steady, the depth target drawn earlier this frame (the pass begins: its targets are noted); the
  next frame all of it is recent again.
- What stays one-shot: a lookup table with a second pass into it in the same frame, generated again at each area load,
  far apart (fewer than `TFR_STEADY` times); a volume generated a few slices per frame (each slice is its own target
  and is rendered once); the item icon capture (a new colour target with the per-frame depth buffer, once); an image
  handed on to a texture at other storage starts again (the per-frame history of its handle is not the new texture's);
  a pass with no target at all.
- The table keeps the steady targets through an area's first frame: 400 per-frame targets, a pause, then 600 targets
  made new are rendered before the old ones come round again.

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

`launcher/telemetry.py` and the session reports' screens of the window (`launcher/ui/app.py`): consent before anything
is sent, the exact bytes sent and shown, the server's 410, silent network errors, no id of the installation, the crash
summary made from a FATAL line, the log lines a comment carries. The window tests need tkinter, numpy and a display:
on Linux under `xvfb-run`, so that no window opens on the desktop. The update-start cases are above; the rest:

- `Server` is a local stand-in for the reports server: it records each request (method, path, headers, body) and
  answers with `status`, changeable between requests. `Base.setUp()` uses the account's folders of a made-up player,
  whatever this machine's are.
- `ConsentTest`. Not asked yet ('') and Off send nothing, also when `send_session` is called directly: the check is
  in `telemetry.py`, not only in the window; the summary is still waiting afterwards. On sends exactly the payload's
  bytes, with the keys `launcher_version` and `summary` only (no id of the installation); the summary is then not
  offered again and nothing is left to send. An earlier version's id is removed from the settings file, the other
  settings stay, and a second removal finds nothing and logs no line. A build without a server (`TELEMETRY_URL` '')
  sends nothing and says so once; the address comes from the environment, where `none` means no server at all,
  whatever is built in. Off in Settings is this version's answer, and the next version asks. The question is due once
  a version while reports are not on: not asked yet; not twice in one version; Don't Send in 0.24 is asked again in
  0.25; on: never asked; no server: no question. The notice of reports turned on is told once and not again.
- `ServerAnswerTest`. A 410 stops reports until the next launcher version: the same version sends nothing, the next
  one sends again. A network error (nothing listens at the address any more) is silent and logs at most five lines;
  the summary is tried again on the next start. A refused report is not offered again, but a busy server's 429 keeps
  it for later. Pressing Send on a comment is its own consent: it goes before the question was answered, with the last
  summary and the log lines asked for; a comment does not use the summary up; a comment of blanks sends nothing; with
  reports turned Off the comment still goes, never with the report the player said no to.
- `CrashSummaryTest`. A hard crash gets a summary from its FATAL line; the same crash is not made into a second
  summary, also after the first was sent. No summary when the game wrote its own for this run, when the log has no
  version line or no FATAL line, or when the launcher already noted the end. A summary of the run before (older than
  `nx2saria.1.log`) does not stop it.
- `LogTailTest`: the last lines within the caps, the last ones kept.
- `WindowBase`. The window is made with `--selftest` (no sounds, no updates), without the search of this PC's folders
  for dumps, and with a server for the whole test in the environment. `WindowBase.close_window()` cancels the window's
  timers, which would run on a closed window, and clears the app's attributes so its pictures go at once, on this
  thread, not on one a later test starts. `WindowBase.logged()` says whether a line of `launcher.log` ends with a
  text.
- `WindowTest`. The view shows the bytes that are sent (once something was sent, an example now). The first start of a
  version asks with Send focused (the default: Enter sends); nothing goes at once and the waiting summary never does;
  the next session's is sent by itself; once on, it is never asked again, not by a new version either. Don't Send
  keeps reports off (the declined summary is not offered again, the same version does not ask again, a game played
  asks and sends nothing) and the next version asks again. "See what is sent" from the question scrolls the JSON and
  goes back to the question, still open. The countdown turns reports on after 30 s without input (the first report is
  the next session's) and the next start says so once. The question's text is whole inside its panel: the title and
  the countdown's line start below the screen's heading (`X(112)`) and the last line ends above the window's foot
  (`X(H - 30)`), top to bottom, counting and stopped (the longer line), whichever row is focused. Any input stops the
  countdown. Closing the question (Esc or B) turns reports on; so does closing the window over it (Alt+F4), also with
  the countdown stopped, and the next start tells. A signal that ends the launcher is no answer: the next start asks
  again. Closing the window elsewhere changes nothing. No question over the first-run screen (no game yet) or in a
  preview (`--screen consent`). The Settings switch from not asked yet: asked again by the next version only. The
  reports screen and the comment are walked with keys: the box takes the keyboard, the text is cut at `TEXT_LIMIT`, B
  or Esc leaves the box only, a comment is no consent to reports, and with nothing to delete only Back is offered.

## `tests/telemetry_patch_sets_check.py`

`telemetry/src/patch_sets.ts` lists the port's patch sets a session report may name (`telemetry/src/validate.ts` drops
any other name). It must hold exactly the names of the `set` lines of `recomp/patches-v*.txt`: a set added there and
not here would be dropped from every report by the server.

## `tests/tensor_lowering_check.py`

Checks the lowered SM86 tensor instructions against results captured on an NVIDIA GPU, in one command
(`python tests/tensor_lowering_check.py [vectors.json] [--xlate EXE] [--case NAME] [-v]`; the `--help` description is
the string `DESCRIPTION`).

`tools/xlate/AmpereLowering.cs` rewrites the SM86 instructions of the game's neural-network passes as Maxwell code:
`Lowerer.Hmma` (HMMA.1688 / HMMA.16816, .F16 and .F32), `Lowerer.Ldsm` (LDSM.16.M88 / MT88 .1/.2/.4), `Lowerer.Movm`
(MOVM.16.MT88) and `Lowerer.Half` (HFMA2, HFMA2.RELU). The script lowers every case of the vector file with
`nx2navi_xlate --lower`, runs the Maxwell words on `tests/maxwell_warp_emu.py` (which executes them as the shader
translator does) and compares with the registers the GPU wrote. Exit status 1 on any failure. The vector file names
the GPU that produced it ("device"). So far that is an RTX 5090 running the compute_86 PTX through the driver's JIT,
as no SM86 part was at hand: the results follow the PTX semantics but need not show SM86's internal precision.

Defaults: the vector file is `work/tensor-gt/vectors.json` of the workspace (a generator beside it writes it: each
case is one SM86 instruction in inline PTX, run on one warp) and the translator is the workspace's
`build/xlate/nx2navi_xlate.exe` (`python tools/build_xlate.py` builds it); the workspace is the directory two levels
above this repository. numpy is needed.

Every case runs several times and must pass each run:

- with the registers the vectors do not set first 0, then 0x7fc0dead, so a read of a register the lowering never wrote
  shows;
- then under @P0 (the guard field of the SM86 word, bits 12-15, set to P0 = 0) with P0 false in every third lane
  (`guard_lanes()`: lanes 1, 4, 7, ..., so every quad and row group mixes both): lanes with P0 get the GPU's result,
  the others keep their old destination values. The other lanes still supply their fragments, rows and addresses,
  which works only if the shuffles run in every lane and just the destination writes take the guard, the rule
  `AmpereLowering.cs` follows.

Each run (`check_run()`, which appends what is wrong to `out.problems`) checks:

- destination registers bit for bit, except that a NaN matches any NaN in a float result (`tests/maxwell_warp_emu.py`
  `same_value()`: SPIR-V leaves NaN payloads to the host, the GPU writes 0x7fff). HMMA gets no tolerance: the GPU
  rounds every element once (f16 or f32 of the exact A*B+C), and so does the lowering for these values, whose sums are
  exact in F32; the largest difference in ulp is reported;
- every other register R0-R236 (guest and uniform registers; `GUEST_REGS` is 237, and R237 up is the lowering's own)
  and P0-P6 keep their values, shared memory is untouched, and the emulator has nothing to report (a shuffle reading a
  lane its guard left out, a PRMT the translator drops);
- urbase, the register the lowering mapped UR0 to, is the highest register the instruction names plus two, rounded
  down to even: `AmpereLowering` `HighestGpr` counted every register of the instruction's fragments and quads.

Read off the lowered words, for both lowerings (`guard_rule_problems()`): every write to a guest register (below R237)
carries the SM86 instruction's guard (the Maxwell guard field), and no SHFL is guarded: a shuffle runs in every lane.
The table beside it lists the Maxwell instructions of `maxwell_warp_emu.TABLE` without a destination register in bits
0-7.

Derived cases (`derived_cases()`), beyond the hardware vectors, for forms the game uses: reference models of
HFMA2(.RELU) and LDSM, each first checked against every hardware case of its instruction (`model_problems()`, exact
bits: the models write 0x7fff for NaN), give the expected values of HFMA2.RELU with an immediate B (opcode 0x831, as
in the game's HFMA2.RELU Rd, Ra, 1, 1, Rc; the immediates 0x3C003C00, (1, 1) as the game's networks use it, and
0x4000BC00, (-1, 2)) and LDSM .M88.4 / .MT88.4 with an offset. MUFU.F16 (LG2 and EX2 of either half, with abs and neg,
on the halves of the HFMA2 case's A register, as the upscaler's colour decode uses it) is checked against a model of
the lowering: no hardware vectors, as PTX has no lg2.f16; the instruction forms were confirmed with nvdisasm.

- `EXIT` (0xE30000000007000F) is Maxwell EXIT: it runs no code and gives the emulator's initial state.
- `operands()`: (destination registers, source registers) one SM86 instruction names, from its fields (the same fields
  `AmpereLowering.cs` decodes). Opcode 0x23C is HMMA (shape bit 75, .F32 bit 76), 0x83B LDSM (count 1 << bits 72-73),
  0x23A MOVM, 0x231 and 0x831 HFMA2 (B a register or an immediate), 0x308 MUFU in register form (the operand is B).
- `shared_memory()`: the case's shared memory as `maxwell_warp_emu.run_case` places it: smem at smem_base, 64 KiB at
  least. `consistency()`: problems of the case itself: regs_out not exactly the destinations, LDSM addresses that
  disagree.
- `half_value()`: an F16 bit pattern as ("nan"), ("inf", sign) or ("num", Fraction, sign). `round_half()`: x (not
  zero) rounded to F16, to nearest even; beyond the range infinity. `fma_half()`: F16 a * b + c rounded once (IEEE 754
  fusedMultiplyAdd, round to nearest even); a NaN result is 0x7fff; an exact zero is -0 only when both terms are -0.
  `hfma2_model()`: HFMA2 (.RELU) on both halves (H1_H0 operands): RELU makes negative numbers and -0 +0, keeps NaN as
  0x7fff. `ldsm_model()`: LDSM by its fragment layouts: out[lane][m] is lane's value of Rd + m.
  `mufu_f16_model()`: MUFU.F16 as `AmpereLowering.cs` `Mufu` lowers it: the half widened to F32 with abs and neg, the
  function in F32 (EX2 2, LG2 3), rounded to a half in the low 16 bits, the high 16 bits clear.
- In `main()` one broken case does not stop the run.

## `tests/tex_compact_check.py`

Builds and runs `tests/tex_compact_check.c` on `runtime/vk/vk_tex_compact.h`: from a table of what lies in the
allocator's blocks, how much of them is holes and could go back to the driver, and which blocks a texture compaction
empties. No GPU, no game files, any Python.

- `random_tables()`: whatever the plan chooses must hold to the rules, in the order the header gives. The tables keep
  the higher blocks emptier and sometimes hold memory of another kind the table does not list. Checked: the room held
  at each step, so it holds for the whole plan; nothing cheaper was left out for no reason (a block that needs no move
  and is emptiable is always taken while the plan has places left and reached its gain).
- The sums of a block (an allocation that names no block is left out).
- Which blocks count: exactly half; one byte over; a small allocation that stays; listed fuller than its size; a block
  of no size.
- A plan: the cheapest block first, the one with a staying allocation never. The same table asked for more than it can
  give; one block at most; moved bytes bounded; a held block.
- Room: what moves must fit outside the chosen blocks. 40 MiB free in a block that is not in the table (it holds no
  texture); with all of the free bytes allowed, and 20 MiB more, both go.
- A block of dropped textures alone needs no room, also when the allocator reports nothing free (memory of another kind
  fills the holes). Of two blocks alike, the larger first.
- The session the statistic was written for: 110 blocks of 64 MiB, a quarter of them nearly empty (6 MiB of idle
  textures; every fourth of them with a VRAM mirror too).

## `tests/tex_copy_mt_check.py`

Builds `tests/tex_copy_mt_check.c` with `runtime/gfx/tex_deswizzle.c`, `runtime/par_jobs.c` and the platform layer
(with `stand_ins()` of `tests/platform_stand_ins.py`): the split texture copy `tex_bl_copy_mt` and `par_memcpy` write
exactly the bytes `tex_bl_copy` and `memcpy` do, in both directions, for levels under and over the split size, with
partial GOBs, block heights and depths, and slices. No GPU, no game files, any Python. `vma_include()` is needed
because `tex.h` includes `vk.h`, which needs `vk_mem_alloc.h` (found as `tests/gpa_check.py` finds it). The C file's
stand-ins are what `tex_deswizzle.c` and `par_jobs.c` need from the rest of the runtime.

The levels: 4096 x 1024 (4 MiB, split); 1000 x 1300 (partial GOBs, split, 1.2 MiB); 1000 x 700 (partial GOBs, under
the split size); 16384 x 512 (8 MiB, wide); 256 x 64 x 8 (a 3D level under the split size); 2048 x 128 x 16 (3D,
split); 64 x 8 (one GOB); 48 x 3 (less than a GOB).

## `tests/tex_detail_check.py`

Builds and runs `tests/tex_detail_check.c` on `runtime/vk/vk_tex_detail.h`: the level `texture_detail` = auto picks
for a video memory budget, on a card with memory of its own and on an integrated GPU (`runtime/vk/vk_texture.c`
`vk_texture_detail_auto()`), for the cards and budgets players reported. No GPU, no game files, any Python.

- The budgets in players' logs of 0.10.1.
- Between the old 8,192 MiB and the new 10,240: the cards of 10 GiB. Low under 6,144 MiB, as before.
- An integrated GPU never gets full, and follows the budget below that.
- No budget reported: full, as before, also on an integrated GPU.
- Uncompressed ASTC textures: one step lower under 20,480 MiB.

## `tests/tex_release_check.py`

Builds and runs `tests/tex_release_check.c` on `runtime/vk/vk_tex_release.h`: how long a released texture is kept, the
bytes the released ones may hold past the size bound, and the clock that turns presents into seconds. The program also
prints a table of the simulated sessions, shaped by the take-back figures of players' logs, against the old and new
defaults. No GPU, no game files, any Python.

The simulation follows `runtime/vk/vk_texture.c` `sweep_pressure()`: once a second (the cache checks every 8
submissions) take-backs that fall due find their entry or make it again, `drop_aged` takes the entries past their
hold, and `drop_stale` takes the oldest entries released 2 s ago or earlier while the released bytes are above
`trl_keep`. The streams are shaped by the figures of three sessions' logs on an RTX 5090 (`vk_tex_release.h`).

- `clock_checks()`: presents at a rate that changes every few seconds (60 to 240 a second), a tick each second, as
  `rate_tick` makes; the age of a present just after a tick is the real one. The hold in seconds does not depend on
  the rate: 180 s at 60 or at 240 presents a second, within the ticks kept (the last `TRL_CLOCK_N` seconds). A run
  shorter than the ring, and an empty clock.
- `rate_checks()`, the rate the short waits go by (`trl_rate_sample`, `trl_rate`, `trl_presents`): the highest of the
  samples; one not taken yet counts as 60, so a run's first seconds never go below it; an area load is one second with
  two presents. 120 presents at 60 a second are 2 s at every rate from 15 a second up (`STALE_FRAMES`, the wait after
  a release); under 15 a second the wait is 15 a second's: 30 presents, longer than 2 s. A tester's log
  (`vk_tex_release.h`): 21 presents a second, where 120 presents were 5.7 s before. The longer waits scale the same
  way: `GPU_STALE_FRAMES` (1800 presents at 60, 30 s).
- `rule_checks()`: the Balanced session's line at frame 7200: 11786 MiB in use of about 31 GiB, 27.3 GB of 71 GB
  commit free. An 8 GiB card in play: 6.3 of 7.5 GiB in use. The default bound by the card's budget (MiB): a
  thirty-second of it, 384 MiB to 1 GiB. What the released textures may hold at their fullest: under 2 GiB on a 32 GiB
  card with room (8 GiB by the rule before, a quarter of the budget and twice that), 384 MiB on an 8 GiB card in play
  (1.9 GiB before).
- `taken_back_checks()`, the take-backs of the logs against the hold. The "vk: released textures frame N" lines of two
  sessions (2026-11-12): take-backs by the time released, in the buckets under 2 s, 2-10 s, 10-60 s, 1-5 min, longer,
  with each bucket's upper edge in seconds (the last open). The run through Hyrule Field that set the bound
  (2026-11-14, the rule before: 180 s, 4008 MiB and twice that with room): MiB and textures taken back by the time
  released, 2-10 s, 10-60 s, later; and the textures loaded anew meanwhile. Nothing those logs took back would have
  gone past the hold: every take-back came before 60 s. In the field run what came back later than the hold is under a
  hundredth of what came back at all, and everything the cache gave back in three minutes is a twenty-fifth of what
  the game loaded anew in that time.
- The simulated sessions. An entry has `rel` (the second of the release) and `back` (the second the game takes it
  back; 0: never); a rule has `cap` (bytes) and `hold` (s); `backs` holds the entries the game takes back, by the
  second it does. A stream is `burst_s` seconds of area changes at `burst_mbs` MiB/s of releases the game keeps
  released, then calm play to `end_s` at `calm_mbs`. Sizes are 0.1 to 1.9 MiB (the Balanced session held 3946 MiB in
  7754 entries, half a MiB each); a tenth hold content only the GPU made. A share `back_pct` per thousand comes back,
  after a time drawn from the logs' buckets past 2 s (2-10 s and 10-60 s about as often); `late` adds take-backs
  `late_s` after the release. `run()` keeps `next` (the first entry not released yet), `head` (the oldest that may
  still be held) and the next take-back; `drop_aged` stops at the first entry younger than the hold, since they are in
  release order; `drop_stale` goes oldest first.
- `session_checks()`: an RTX 5090 with a 31 GiB budget (until 0.11.0 a quarter of it, 4008 MiB, the logs' figure, and
  180 s; now `trl_cap_mb`) and an 8 GiB card with no room in play. The streams: the Balanced session's second line
  (3.2 GiB released and kept within 35 s, then 10 minutes in one area); a long session (an area change every few
  minutes, shown as a slower stream throughout); a teleport tour (4 minutes at the burst rate); the run through Hyrule
  Field (the game lets go of about 130 MiB a second for three minutes). What the cache holds at its fullest is what
  the session costs in video memory: never more than what it may keep and what two seconds release (`drop_stale`
  leaves the entries of the last two seconds), on either card, and never more than the rule before held; where that
  one filled up, less than half of it. What it holds once play is calm is what the last hold released. A take-back
  within ten seconds of the release (turning round) finds its texture wherever ten seconds of releases fit in what the
  cache may keep; on the 32 GiB card that is every scenario here. What it makes again is never more than what the game
  takes back at all (a fiftieth of the stream here). A game that takes textures back late, in calm play where the
  bound is far: at 50 s nothing goes before (the hold is 60 s), at 90 s the hold has dropped them.
- `hard_checks()`, past the hard count (`trl_hard_class`, `trl_hard_pick`): at the session's end the release order
  mixes render targets (GPU), intact entries and a few overwritten ones, and the render targets are the oldest; the
  rule before took them first. With only render targets and sampled ones the render targets go, in release order;
  with none unsampled, nothing.

## `tests/text_speed_check.py`

Builds and runs `tests/text_speed_check.c`: the `text_speed` setting (`runtime/patches/text_speed.c`) and the
replacement words of the patch set `text_speed` (`recomp/patches-v111.txt`), lifted by `recomp/lifter.py` as the
generator lifts them into `sites.c`, against a stand-in message window and typewriter. No game files: the lifted words
go to the test's build folder.

In the Python file `site_words()` returns the set's replacement words at a site, after checking the site has only them
and the original word: `tbz w20, #0, #0x774b80` (0x36000074) at the skip site and `ldr s1, [x19, #0x4c]` (0xbd404e61)
at the speed site. `lifted()` writes a C function running the words at the site: the `adrp` of the patch page is left
to the caller, who puts the page's address in that register, and a last conditional branch (`tbz w9, #0`, 0x36000069,
with the original's target) is left to the caller too, who reads its register.

In the C file `site_skip` is main+0x774b74 without its `adrp` (x9 = the page) and its last `tbz w9, #0`; `site_speed`
is main+0xafe1d8 without its `adrp` (x22 = the page). `set_on` is the patch set, on at the start as its default. The
message window has +0x34a = 1 when its message may not be skipped; a typewriter has its speed at +0x4c. `run_skip()`
runs the skip site for the window and its typewriter, returns the decide bit the site's `tbz` tests, and checks that
only x9, x10 and the page's typewriter word changed. `run_speed()` runs the speed site for a typewriter, returns the
speed it loads into s1, and checks that only x27, s1, s2 and s8 changed.

- normal: the set goes off, the page holds no factor and no skip. An unknown value reads as normal.
- fast: the set on, the factor less one on the page, no skip. A message the player may skip: the window's typewriter
  goes on the page and types faster; the decide bit is the player's own. Any other typewriter keeps its speed. A
  message the window does not let the player skip: no typewriter on the page, the game's speed.
- instant: the set on, no factor, the decide bit set for the skip test (whose +0x34a test is the game's own).
- Back to normal: the set off again (the sites run the original instructions), the page cleared.
- With the argument `kept-off` the set starts off (`NX2SARIA_TEXT_SPEED_PATCH=0` or a mod on its sites), and no
  setting may switch it on or touch the page.

## `tests/thread_end_check.py`

Builds and runs `tests/thread_end_check.c` (with `stand_ins()` of `tests/platform_stand_ins.py`): what a thread keeps
for itself is given back when its function returns (`runtime/platform/win32.c` `thread_main()`,
`runtime/platform/platform.h` `plat_thread_end_hook`). 416 threads sleep through `plat_wait_ns` (1.2 ms, over a
millisecond: the thread's own timer), read a file and end, half joined and half detached; the end hook must run once
on each, and on Windows the process must not hold more handles afterwards than a few threads' worth (each thread made
a timer and a read event: left open, 400 threads would hold 800 more handles). Off Windows there is no handle per
thread and the hook is what is checked. A failed read is counted as a failure; the wait for the end sleeps 100 ms more
so the last threads leave their start function and the system lets go of them. `rt_log()` takes the platform layer's
lines. No game files, no GPU.

## `tests/tracker_api_check.py`

Builds `tests/tracker_api_check.c` (`runtime/diag/tracker_api.c` over the real platform layer, with a guest memory of
its own at the game's addresses) and talks to it as a map tracker does: newline-delimited JSON requests, one at a
time, each answer matched by its id. No game files and no game build: the module layout and SDL's clipboard header are
stand-ins written to the test's build folder. `TRACKER_API_CHECK_LOG=1` prints the server's log on success too.

It checks the answers a tracker relies on (status, read_memory, search_memory, the port's own `nx2saria.state` and
`nx2saria.game_data`, over a stand-in game-data manager: every kind, keys, digests, and each way of the stand-in's to
spoil a read, refused instead of answered with made-up values), what is refused (bad parameters, memory the game
cannot read, the host's own memory beside the game's, unknown and writing methods), what closes a connection (a first
byte that is not '{', such as a web page's HTTP request or TLS handshake, JSON that is not one object, numbers JSON
does not spell that way, an overlong id, a line of 4096 bytes, a request left unfinished), that searches never make a
page of the heap resident, the searches' minute of bytes, the client limit, and the log lines the bench reads (the
start line, client lines, first answers and first refusals).

In the Python file: `HOST` (0x2000000000) is the host's own memory in the C file; `SEARCH_MIB_PER_MIN` (512) is the
searches' minute in this build (`runtime/diag/tracker_api.c`, `SEARCH_BYTES_PER_MIN`). `Client` is a tracker's
connection: one request in flight, answers read line by line; `Client.closed()` is 1 when the server closed the
connection (EOF within the timeout). `records()` gives a game_data answer's records as (hash, the record's bytes after
the hash); `none_open()` reads the server's log until a client's closing line says no client is left open, False at
its end. In `run_checks()`: main is found by its MOD0 header, as a tracker finds it (a range below the module window
first, no hit, then all regions); ids: a string comes back as sent, no id gets an answer without one, two requests in
one write get two answers; four clients at once and a fifth refused, after the server has let an earlier one go (on a
busy machine its thread could still count it); a tracker checking whether the port is taken (connect, close, no
request), three of which log one line; what closes a connection at once when the first byte tells; a request that
keeps getting a blank now and then is still unfinished; the searches' minute: main's 64 MiB a search, until a search
stops short. `run_game_data_checks()`: `nx2saria.game_data` is refused before the game data is there, then each kind
as a tracker reads it, then each way the game data can fail to read: refused, never answered with made-up values,
nothing of the host read. `main()` waits half a second at the end for the client threads to log their closing lines.

The C program's guest memory: main's window at 0x7000000000 with a MOD0 header where 1.1.1's main has it (`MOD0_AT`,
main+0x3c4705c; main+0 holds 1 and main+4 the MOD0 offset; `MAIN_COMMIT`, main's first 64 MiB, holds it), and a heap
of 16 MiB at 0x1030000000 (the stack and heap block of a run with the tracker API on) with a hole the lazy heap never
committed (`HOLE_AT`: [4 MiB, 5 MiB), reserved, never committed), a page nobody may read (`DARK_AT`, one committed
page) and pages never written. Beside them, at 0x2000000000 (`HOST_AT`, outside every guest region), memory of the
host's own holding a secret: the game's memory map answers for it as taken memory the game can read, as
`runtime/memory.c` `query_locked()` does for every host allocation below 2^39 (`mem_query()` here: main read-only, the
heap read-write, the rest taken by the host). The search needles: one inside one piece of the heap, one across the end
of a search's first copy, and one that runs into the dark page, never a hit. The save folder is as a Windows profile
under an accented user name has it: backslashes, a quote, UTF-8. `dbg_player_rot()` answers a quarter turn about the
vertical. `rt_log()` writes one whole line at a time, as the runtime's log writes it (stdio locks the stream for the
call).

The program prints "READY <port> <resident heap pages>" once it listens, logs every `rt_log` line to stdout, and
prints "RESIDENT <resident heap pages>" (the heap's pages that hold memory now, by the platform's own answer) and ends
when stdin closes. The game data is not there at first, as before the game makes it. A line "LOAD" on stdin puts a
game-data manager in the heap where version 1.1.1 has one (`**[main+DBG_GAME_DATA_SLOT]`), with a few values of each
kind the API answers, and prints "LOADED"; "EMPTY" before it puts the manager there with its sections still empty, as
the game has it before it loads a save (EMPTIED). Then each line spoils one thing and prints its answer word: "POINT"
turns the String64 and String64Array text pointers to the host's own memory (POINTED), "MANY" gives the String64Array
value 65 texts (MANIED), "DUPHASH" gives Int's second pair the first one's hash (DUPHASHED), "DUP" gives it back its
hash and the first one's index (DUPPED), "UNSORT" swaps two placed objects' keys (UNSORTED), "ZERO" clears the Enum
entry's vtable, as an entry the game is still making has it (ZEROED), and "BREAK" gives the Bool kind's middle entry
another vtable, as a section read at the wrong stride would have (BROKEN).

The stand-in game data is 1.1.1's manager, each kind's section at its number times 0x60; `GD_AT` (heap + 0xc00000)
holds the slot's target, then the manager, then the kinds' entries and tables, and is made resident at the start so
LOAD changes no count of resident pages. The kinds' vtables: Bool's and Int's are 1.1.1's (`tracker_api.c` `gd_kinds`
checks them), the others any in main. A kind's section has n entries of `stride` bytes, each starting with the kind's
vtable, and a {hash, index} table of 2n pairs, each placed as the game places it: from hash % size on, at the first
empty pair (Int's table, after its 2 entries: 4 pairs at hash % 4, 0x04735978's first, index 0, 0xfbe01da1's second).
In `gd_load()` some entries carry other bits that are not the value, the save index and other bytes are left out of
the answer, a u32 count has other bytes after it, and the slot at main+`DBG_GAME_DATA_SLOT` is written last: the game
data is there from then on.

## `tests/tsan_build_test.py`

Checks the bench sanitizer compile and cache boundaries without game data.

## `tests/ui_text_ids_check.py`

The text catalog of the menu, the overlay and the software keyboard (`runtime/ui/ui_text.h`, `runtime/ui/ui_text.c`):
no id without a use, no string without an id. It reads the sources as text; no compiler, no game files.
`tests/run.py` runs it with `--with-self-test` (the check, then the planted cases).

- An id is used when a source under `runtime/` or `tools/` names it (not `runtime/third_party/`; in `ui_text.c`, a name
  that is not a `[UI_...] =` designator of a table or of the `LANG_NAMES` and `SWKBD` macros, as in `all_langs[]`), or
  when an entry of `ARITHMETIC` marked as showing text reaches it. A name in `docs/` or `tests/` is not a use. An
  unused id fails the check: it goes with its strings in every language.
- `ARITHMETIC` is every place that computes an id from another or compares ids by order: the file, the expression as
  it stands in the code, the first and last id it can give, whether it shows their text, and what it is for. The bind,
  key, gamepad and preset tier labels are reached this way; the `static_assert`s of `runtime/ui/binds_page.cpp` and
  the loops of `runtime/hle/ov_probe.c` need the order but show nothing. An entry whose expression is no longer in its
  file fails, and so does a line of C or C++ with `+`, `-`, `<`, `>`, `<=` or `>=` next to an id's name that no entry
  holds, so new arithmetic has to be declared with its range. Comparisons with `UI_TEXT_COUNT` (the bound of the
  tables) and `==` are not arithmetic. Arithmetic on a variable that holds an id (`label + 1`) has no id's name beside
  the operator and is not seen: the tree has none (every conversion of a `UiTextId` to an integer in the C++ sources
  was listed from the compiler's syntax tree when the check was written).
- The tables `text_en`, `text_ru`, `text_pt_br` and `text_zh_hans`: a string for an id the header does not declare
  fails, as do two strings for one id in one table (the compiler keeps the last without a word), a string in a
  language for an id English has none for, and an id without an English string (it would show as nothing in every
  language). A missing Russian, Brazilian Portuguese or Chinese string is allowed, since `ui_text_in()` shows the
  English one; the summary line counts them per language.

- `FILE_NAME_GLYPHS`: the characters `runtime/ui/ui_text.c` `file_name_glyphs_zh_hans` must hold, and
  `ui_text_visit()` must hand that list to the font builder (`glyph_problems()`), so the Simplified Chinese glyph set
  does not shrink below it ([ui.md](ui.md), `ui_text_visit()`).

### `check()`

Takes the header, the catalog source and the other sources as text, so the self-test plants its faults in copies in
memory. Returns the problems and the counts of the summary line.

### `self_test()`

A planted id with strings in the four tables that nothing names; a string for an undeclared id; an id without its
English string; a second string for one id; `UI_MENU_ON + k` in a source; a declared expression renamed in its file:
a character taken out of the
glyph list; a catalog walk that no longer hands the list over: each must fail with its own message. A dropped Brazilian Portuguese string must pass and raise that language's count
by one.

## `tests/up_defer_check.py`

Builds and runs `tests/up_defer_check.c` on `runtime/vk/vk_up_defer.h`, the list of what the upscalers let go of while
the GPU may still use it (`runtime/vk/vk_dlaa.c`, `runtime/vk/vk_ffx.cpp`, `runtime/vk/vk_ffx4.cpp`): it is released
only once the GPU timeline has passed its last submission and, for what a present can read, once enough presents went
by. Built as C, and as C++ since the two FSR files include the header from C++. No GPU, no game files, any Python.

- A DLSS feature last evaluated in submission 10, a context last dispatched in 12, a never-used one (fence 0).
- An image a present may read: the fence alone does not free it, the present count must pass too. Both gates: the
  later one decides.
- Order: the ready ones in the order put, the rest kept in order.
- `take_all`: the device is idle, everything goes whatever its gates.
- Full: the caller is told, and nothing is overwritten.
- A callback may put another entry (a release that lets go of something else): the new entry waits for the next take.

## `tests/upscaler_idle_check.py`

Static checks of the upscalers' waits for the device (`runtime/vk/vk_dlaa.c`, "what the upscalers let go of while the
GPU may still use it", `runtime/vk/vk_ffx.cpp`, `runtime/vk/vk_ffx4.cpp`, `runtime/vk/vk_fsr3.c`). No GPU, no game
files, any Python (`upscaler_idle_check.py [repository root]`). It fails when:

- `vkDeviceWaitIdle` is called in `vk_dlaa.c` outside `vk_upscaler_device_idle`, in `vk_ffx.cpp` outside
  `vk_ffx_probe_cache` (the `--probe-ffx-cache` path), or anywhere in `vk_ffx4.cpp` or `vk_fsr3.c`: each such wait
  stalls the replaying thread with the queue lock held (`WAIT_ALLOWED`);
- `vk_upscaler_device_idle` is called outside `vk_upscaler_settle` and `vk_upscaler_gpu_wait` (`IDLE_ALLOWED`: the
  rollback switch `NX2SARIA_UPSCALER_IDLE_WAIT=1`, a full list, a refused creation's failed wait);
- one of the places that let something go (`LET_GO`: `vk_dlaa.c` `free_size`, `free_snap`, `stale_sizes`,
  `release_kept`, `hudless_from`, `pb_images`, `retire_idle` and `evaluate`, `vk_ffx.cpp` and `vk_ffx4.cpp`
  `context_for`, `vk_fsr3.c` `ctx_for`) does not call `vk_upscaler_let_go`, which frees what the GPU is past and which
  the rollback switch turns into a wait for the device;
- one of the places that create a DLSS feature or an FSR context (`REFUSED`: `vk_dlaa.c` `evaluate`, `vk_ffx.cpp` and
  `vk_ffx4.cpp` `context_for`, `vk_fsr3.c` `ctx_for`) does not call `vk_upscaler_gpu_wait`: a creation refused while
  what was let go of is still in flight then fails for want of its video memory, where the wait for the device of
  0.10.1 had freed it first;
- one of the places that let go of what the GPU may still use (`NO_FREE_NOW`: those above, the helpers that hand it to
  the lists, `vk_dlaa.c` `retire_size`, `retire_feat`, `retire_snap`, `retire_hl` and `retire_pb`, `vk_ffx.cpp` and
  `vk_ffx4.cpp` `retire_up`, `vk_fsr3.c` `retire_ctx` and `vk_fsr3_upscale`, and the FSR files' trims and
  `release_idle` calls) frees something at once: a destroy that takes an idle device (teardown) or a half-made object
  that no submission named yet (`FREE_NOW`: `vk_dlaa.c` `destroy_size`, `destroy_snap`, `hl_destroy`, `pb_destroy`,
  `feat_release`, `N.release`, `vkDestroyImageView`, `vmaDestroyImage`; the library files' `A.destroy`; `vk_fsr3.c`
  `free_ctx`, `vkDestroyImageView`, `vmaDestroyImage`), which on a GPU still running a frame is a use after free.
  `vk_dlaa.c` `hudless_from`, `pb_images` and `snap_for` let go of the old images and then make new ones: they may
  destroy only after their first `image2d` call, where the failure path destroys what of the new images was made and
  never used;
- a whole list is taken whatever its gates (`upd_take_all`) outside teardown (`vk_dlaa.c` `vk_dlaa_release`,
  `vk_ffx.cpp` `vk_ffx_release`, `vk_ffx4.cpp` `vk_ffx4_release`, `vk_fsr3.c` `vk_fsr3_release`) and a full list's
  fallback (`vk_dlaa.c` `dead_put`, the library files' `retire_up`, `vk_fsr3.c` `retire_ctx`), or there before
  `vk_upscaler_settle`'s wait for the device;
- `NX2SARIA_UPSCALER_IDLE_WAIT` has no `sections[]` entry in `cfg.c` or no line in `nx2saria.ini.example` (its
  `known[]` name and its `metas[]` row, and a `//` that would hide them, are `tests/cfg_rows_check.py`'s);
- `perf.c`'s `mark_name` has another count of names than `host.h` has `PERF_M_*` kinds: a kind without a name prints a
  null string on a slow frame's line;
- `vk_dlaa.c` `pb_images` does not note `PB.used` after the last call that clears `PB`: `idle_release` then took the
  images it had just made for 600 presents unused and freed them while the frame that used them could be in flight.

How it reads the sources: `blank_comments()` gives the source with comments and string literals replaced by spaces
(newlines kept), so offsets stay and only code is read. A function definition is found at column 0: a return type,
the name, the parameters and the body's brace (the runtime's style); declarations end with ';' before any '{'; in the
blanked code `extern "C"` reads as `extern " "`. `functions()` returns [(name, body start, body end)] of each function
defined; `calls()` [(line, enclosing function)] of each call of a callee, its definition's own head and declarations
left out; `call_offsets()` the offsets of each call of a callee between two places.

## `tests/usnap_concurrency_check.py`

Builds the real `runtime/vk/vk_usnap.c` concurrency probe (`tests/usnap_concurrency_check.c`) on Linux, with optional
ThreadSanitizer; the `--help` description is the string `DESCRIPTION`:

```sh
python tests/usnap_concurrency_check.py --vma-include <vma-dir> --work <scratch-dir>
python tests/usnap_concurrency_check.py --cc /path/to/clang-20 --vma-include <vma-dir> --work <scratch-dir> --tsan
```

No game files or GPU are needed. The probe exercises the real uniform snapshot drop, lookup and diagnostic paths
concurrently. The replay stream position (`test_pos`, the caller's place in the stream, `vk_exec_stream_pos`), the
pool lookup, the hit accounting and the platform lock are stand-ins, so it checks the actual snapshot data structure's
concurrent drop, find and why operations, not replay order, GPU synchronization or fences. `vk_usnap.c` takes only
`drop_lock` along the exercised paths; the other runtime boundaries do not need a device. The Vulkan and VMA headers
are needed to compile the runtime's types. Phase barriers make both threads progress between blocks without ordering
their accesses within each block; they do not force individual operations to interleave: an empty TSan report is not
a proof of every schedule. The bench uses Clang 20; pass its path with `--cc` if needed.

In `reader()` the range asked about is outside all regions and writes, so `why` scans the list while the other thread
grows it, and `dropped` can change between the two separate calls. In `main()`: a lookup before the drop's place in
the stream may use the snapshot; at or after it, it may not. `why` keeps its existing diagnostic behaviour and reports
any drop, including one later in the stream than the caller.

## `tests/usnap_order_check.c`

The order of uniform snapshot drops between replay chunks, on the real `runtime/vk/vk_usnap.c`, without a GPU;
`tests/replay_hazard_check.py` builds and runs it.

A draw that writes guest memory as storage drops the snapshot regions it touches (`vk_usnap_drop`) from its place in
the stream on (`vk_exec_stream_pos`: its group and chunk), and a later uniform binding of that memory then reads the
live bytes, which hold the GPU's result when the draw runs. The chunks of a group, and with `NX2SARIA_REPLAY_PIPE` the
chunks of the groups after it, record at the same time. Two orders are forced with barriers:

- early: a later chunk of the same group has dropped the region already when an earlier chunk looks it up. The earlier
  draw must still bind the snapshot: live memory holds what the game wrote after the submit. In the case the writer is
  chunk 1 of group 5 and the reader chunk 0 of the same group; the reader's draw runs before the write, and the right
  value is the snapshot.
- stale: an earlier chunk's drop has not run yet when a later chunk looks the region up, so the later draw binds the
  snapshot and reads the bytes as of the submit instead of the earlier draw's result (the right value is `NEW`). This
  one is known and left open ([thread-sanitizer.md](../thread-sanitizer.md), "Replay hazards"): the runtime counts
  the chunks' drops into copied ranges, its precondition, and none happened in the scenes measured. The probe expects
  the wrong read, so a change that fixes it shows here. The controls run the drop first.

Each case also runs in the stream's own order (the control, which must read the right value). The value a draw reads
is the snapshot's bytes when `vk_usnap_find` hands out the copy, else what live memory holds when the GPU runs the
draw, in stream order. The chunks' drops into copied ranges are counted; the submitting thread's are not.

The values: `OLD` (0x11111111) is guest memory at the submit, the snapshot's copy; `LATE` (0x22222222) what the game
wrote into the block after the submit, before the GPU ran the draws; `NEW` (0x33333333) what the storage-writing draw
stores. `test_pos` is the thread's place in the stream, group << 5 | chunk; `SUBMITTER(groups_out)` is the submitting
thread past that many groups (chunk 31). A step has `writes` (the draw writes the block as storage and drops it, else
binds it as uniforms), `first` (takes its step before the other thread's: 1, or after: 0), `live_at_gpu` (live memory
when the GPU runs this draw, in stream order) and `read` (the value the draw reads); the uniform binding reads
`vk_usnap_bytes`: the snapshot, or live memory. The case helper takes the writer's and the reader's stream places and
who steps first, and returns the value the reader's draw read; with `expect_right` the read must be the stream
order's value, else the case is the known open one and must read wrong.

## `tests/validate_shader_cache_test.py`

`tools/validate_shader_cache.py` on a fake shader cache: the entries the game would refuse (a cut or zero-filled
module, another magic number, a reflection that is not one JSON object naming its stage, a missing half of a pair) are
named, packed and loose entries are both read with the loose one standing for its packed copy (a newer translation
beside the pack's bad copy), `--every` samples, and, where spirv-val is installed, a whole module that is not valid
SPIR-V is refused by it, while the game's checks alone take it. No game files.

`module()` is a compute shader that does nothing, SPIR-V 1.3: ids 1 main, 2 void, 3 the function type, 4 the label;
its instructions are OpCapability Shader (17), OpMemoryModel Logical GLSL450 (14), OpEntryPoint GLCompute %1 "main"
(15), OpExecutionMode %1 LocalSize 1 1 1 (16), OpTypeVoid (19), OpTypeFunction (33), OpFunction (54), OpLabel (248),
OpReturn (253), OpFunctionEnd (56). `cut_module()` is a module whose last instruction says three words and has one:
what a write cut short leaves. The cut cases: not a whole number of words, and a last instruction that runs past the
end.

## `tests/vkmem_lifetime_check.c`

The lifetime of pool records and VRAM mirrors against a reader on another thread, on the real
`runtime/vk/vk_memory.c`, without a GPU; `tests/replay_hazard_check.py` builds and runs it. Each case runs in a child
process built with AddressSanitizer, so a use after free ends that child with a report instead of passing unseen (a
sanitizer report ends the child with 66, a wrong state with 1).

- fini: the replay resolved an address of a pool (`vk_mem_resolve`) and uses the record after the game's thread
  finalized the pool (`vk_pool_fini`) in between. A resolved pool is marked used, and `pool_fini` then keeps the record
  until the submission being recorded completes. The control clears the mark first, as if the protocol did not hold:
  the record goes at once and the reader's use is a use after free. In the case the replay holds the record while the
  game finalizes the pool; the reader then does the rest of the binding, as `bind_range` does after its resolve (the
  mirror use and the twin), and a new resolve finds that the index no longer has it. The record must still be there
  then, and go once the submission being recorded completes.
- evict: a replay worker uses mirrors while another evicts the idle ones (`mirror_evict`, under `use_lock` as
  `mirror_create` calls it while chunks record). A mirror the submission used is not idle, and the lock serializes the
  rest, so a use never gets a mirror half taken down. The control breaks both (any mirror may go, `min_idle` 0, and the
  use takes the unlocked path; its "submission" never ends): a use that passed the state check then reads the buffer
  the eviction just cleared. The case runs submissions one after another (`msub` moves on by 200 between them, with no
  chunk recording, as at `vk_mem_mirror_begin`). In each, a worker uses pool A at every draw and pool B in every other
  submission, through `vk_mem_use` as a chunk does (`use_lock` on the slow path), while the other worker makes 200
  evictions under `use_lock`. An evicted pool's next use makes its mirror again (the real `mirror_create`). Every use
  must get a buffer that is not destroyed while its submission records. The submission then goes to the GPU and
  completes; the next submission's copies are none in this harness. The case must evict for its result to mean
  anything (else it returns 2).

The runtime around `vk_memory.c` is stubbed: `tid_self` is nonzero per thread for the recursive lock; the timeline is
`tl_issued` and `tl_done` (submissions issued, and done on the "GPU"), and completions run when the harness moves
`tl_done` on (`complete_to`: the GPU reaches a value, the completions up to it run, then a collect frees what they
retired); `shared_here` is `vk_exec_shared` (this thread records a chunk beside others). `BASE` (0x200000000) is
inside the guest address range the write watch covers (`SNAP_REGIONS`). The mirror state is set as `mirror_create`
leaves it, without its copies: on the list, synced at a given submission.

## `tests/voice_language_check.py`

Checks the 1.1.1 voice language patch's lifted call and callback against stubbed game-data reads;
`tests/voice_language_check.c` is the voice resource's update with the guest option calls stubbed. Its cases: the
saved option is not ready; a successful read of zero still has a value; the stock write triggers the refresh and
advances our observation; a new saved value without a write notification; an unavailable lookup leaves the stock write
result intact; a temporarily missing value does not repeat the refresh.

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

## `tests/warm_hint_check.py`

Builds and runs `tests/warm_hint_check.c` on `runtime/vk/vk_warm_hint.h`: what the library warm's log adds when the
driver's cache of stage libraries is cold (`runtime/vk/vk_pipeline.c` `lw_main()`), by GPU vendor, by platform and by
why it is cold. Both platforms' text is checked through `lw_cold_hint`'s `windows` argument; `LW_ON_WINDOWS` on the one
built for (the game passes the platform it was built for).

- Windows, cold at a start with no finished warm before it (a driver update, or the build's preparation step, which
  writes no `shader-warm.txt`): on NVIDIA the driver's Shader Cache Size limit is named as another cause, with the
  setting.
- Windows, cold right after a finished warm on the same driver: on NVIDIA that is the cache size limit, said plainly.
- Warming given up after two cold starts in a row: every vendor, on every platform, is told how to warm again once the
  cache can hold the libraries; NVIDIA on Windows also names the setting that lets it.
- Elsewhere there is no NVIDIA app and no Shader Cache Size setting: the driver's cache is set otherwise there, so no
  case names them.
- A warm `lib_warm` = force started on a cache that is not cold: no cause to name, on any vendor or platform.

## `tests/water_projective_check.py`

The water corrections the runtime makes in memory (`runtime/gfx/spirv_water.c`) against the same steps of
`tools/projective_recipe.py`, on two made-up modules: a vertex stage that writes a position and a location 0 output,
and a fragment stage that reads a location 0 input without perspective. Each side's result must be the same words, and
valid SPIR-V. A module of another shape must be refused, and each edit refuses the other stage's module. It needs
spirv-as and spirv-val (MSYS2 UCRT64 spirv-tools); `python tests/water_projective_check.py [--cc CLANG]`.

## `tests/wb_lazy_check.py`

Builds and runs `tests/wb_lazy_check.c` on `runtime/wb_lazy_choice.h`: whether the lazy write-back runs, by the
player's setting, the platform and the GPU driver (`runtime/vk/vk_memory.c` `vk_mem_lazy_decide()`). It needs a C
compiler only: no Vulkan headers (the `VkDriverId` values of `vulkan_core.h` are copied), no GPU and no game files.
Every platform's and driver's choice is checked through `wb_lazy_choice`'s arguments; `WBL_ON_WINDOWS` on the one
built for (the game passes the platform it was built for).

- Nothing set, Windows: on, except on AMD's own driver (both of its branches report this id). Nothing set, elsewhere:
  off on every driver.
- An empty value is no value (`cfg.c` `add`: "key =" leaves the setting at its default).
- What the player sets wins on every platform and driver, and the reason says it was set. Any value but 0 counts as
  on, as `cfg_bool` reads it for `memory.c` `mem_init`.
- The exception is the driver, not the vendor: another driver for AMD cards follows its platform's default.

## `tests/wide_balloon_check.py`

Checks the 1.1.1 speech balloon patch and its horizontal anchor on synthetic inputs; it builds and runs
`tests/wide_balloon_check.c`.

## `tests/wide_bars_check.py`

Checks the menus' black bars: the patch set `wide_bars` (`recomp/patches-v111.txt`) and
`runtime/patches/wide_bars.c`. No game files: the module layout is a stand-in written to the test's build folder.

The set's one site, lifted by `recomp/lifter.py` as the generator lifts it: the screen setup's load of the vtable at
main+0x774068 (`SITE`; `ORIGINAL` 0xf9400268, `ldr x8, [x19]`), kept, then a call of the patch page's +0xef0 (`HOST`,
the page's host slot), which `patches.c` registers for `wide_bars_screen_host` and no other set or host function
takes. In `check_site()` the FOV set watches the same screen setup even when the bars are disabled.

Then `tests/wide_bars_check.c` builds `wide_bars.c` with the rest of the runtime stubbed and plays presents against
stand-in interface screens; the argument `no-set` runs a start whose generated C lacks the set. `test_main_base`
(0x7000000000) is `layout.h`'s `MOD_MAIN_BASE`: the vtables are numbers compared, never read. `SCREEN_BASE`
(0x4bec998) is what a destroyed screen's vtable becomes (its destructor's base class). `full_lines` counts the log
lines about a screen left out of a full table. The site is called with the screen in x19 and x8 just loaded from it;
every register must be the same afterwards.

- Before `wide_init`: nothing to black out, whatever is up.
- The screens the game sets up: the two menu backgrounds and the loading fade are watched, the others not; a screen
  set up again is still watched once. In play every watched screen is closed: no bars.
- The pause menu: black from its opening to its close. The system menu's background is the other screen of the class.
- The wide loading fade only needs side bars while fully open. An event fade also keeps its margins black; other
  screens do not.
- A movie, also under a menu; the sides come back once both are gone.
- A screen the game destroys: its vtable becomes the base class's, its state no longer counts and its entry frees;
  another menu background set up at the same address is watched again.
- A full table: one line, and the screens already watched still count; a destroyed screen's slot goes to the next
  screen set up.
- A tall picture watches fades and movies without clearing their full-height image margins.
- The lines about the sides stop after `BARS_TOLD`.

## `tests/wide_hud_check.py`

Builds and runs `tests/wide_hud_check.c`: `runtime/patches/wide_hud.c`, the HUD's edge elements moved out to the sides
of a picture wider than 16:9 (`NX2SARIA_WIDE_HUD` = edges), and the menus laid out over the whole picture, on layout
files and archives the test makes itself. No game files. The `--help` description is the string `DESCRIPTION`.

With `--romfs DIR`, also on the game's own archive: the folder holds `romfs.bin` and its `index.json`, and the program
reads the archive, `Pack/ZsDic.pack.zs` and the resource size table from `romfs.bin` at the offsets the index gives
(`game_args()`), edits the archive for several shapes of picture and after the action prompt's edit of the same
archive, prints the log lines of the 3440x1440 and 21:9 edits, and raises the table's entry for the archive the menus'
redesign makes larger. The pause menu's backdrop drawn in pieces (`runtime/patches/wide_hud.c` `split_layout()`) is
compared byte for byte with `tools/layout_dump.py` `split_picture()`, the reference, at u 640 and 1920:
`reference_splits()` reads the game's first `PlusMenu_00.buil` (the zstd command unpacks the archive), splits it with
the same pieces (the table in the script is `wide_hud.c` `menu_bg_pieces`: `P_MenuBg_00` of `PlusMenu_00` in pieces,
as `split_picture` takes them) and writes the results to `build/test/wide_hud/` for the program. Nothing else is
written.

The program is built with the undefined-behaviour sanitizer (traps), with locals that start as a pattern rather than
whatever the stack held, and with AddressSanitizer where the toolchain has it (not MSYS2's). `runtime/simd_fp.h` `FOK`
ors two compares on purpose, and clang 18 warns on it.

### `tests/wide_hud_check.c`

The layout files are made by the test from the format as the notes of `runtime/patches/wide_hud.c` describe it, with
the panes, values and animation curves the game's 1.1.1 `PlayerStatus_00`, `MapMenu_00`, `NumDisplay_00`,
`LoadingFade_00`, `PlusMenu_00`, `MenuOverlay_00`, `PaShortCutSlideList_00` and `PaItemMenu_00` hold, so the reader is
checked against a writer of its own; the archives and zstd dictionaries are made in the test too. It prints each
failed check and exits 1 on any.

The writer:

- The tables of panes and curves leave their trailing fields out where they are 0 (a pane's width, a curve's slopes),
  as most of the game's panes and curves have them. `Z` (-2.842171e-14f) is how the game stores some zeros. A pane has
  `parent` (an index in the same table, -1 for the root) and `w` (the width, written when not 0; the record keeps its
  fill pattern there otherwise). A curve is of one component (0 x, 4 y) of a pane's property (0 scale, 1 translate, 4
  alpha): its keys' values and slopes (0 when not given); a constant is of a component of a pane's property. A shift
  is of the HUD alone (out units), of the loading icon alone (squeeze q), or both.
- What `wide_hud.c` and `action_prompt.c` reach in the runtime is stubbed, and every log line is kept; a helper gives
  the kept line that starts with a prefix, NULL when none does.
- `make_layout()` writes a BFFH layout from `top` = 0x310, past the header, whose last slot (the per-material array,
  `plus_records()`) ends at 0x310; one track per (pane, property) in the order the curves and then the constants name
  them; the 6-byte target code of a pane's own property is as the game's files have it. It records where it put
  things in the last file it made: each pane's record, and each curve's values block.
- `plus_records()` appends the records `PlusMenu_00` holds past its panes and animations, as the game's 1.1.1 file has
  them where the backdrop's split reads them (`wide_hud.c` `split_layout()`): each pane's subtree end and kind, a hash
  beside each name (`wide_hud_name_hash`), the pictures and materials of `P_InfoBg_00` and `P_MenuBg_00` (a picture
  record with its lists inside its own 0x88 bytes, a material with one texture map right after it), the per-material
  array at 0x300 (`P_MenuBg_00`'s entry 7, to see the new ones take it), the four pages' parts (their pane at +0x40)
  and the two captures (at +0x04) with their panes' indices, and the string pool section reaching the end of the
  file. It returns the file's new size.
- The game's layouts, as far as the edit reads them; among them the pause menu's map and the world map, which the
  menus over the whole picture lay out wider (`P_PicField_00` has its own scale, 0.75: a move in its parent's space
  all the same).
- `make_sarc`: a SARC of one other member, then the given members in order; `member_at` is where each member starts in
  the last archive made. A helper points a member's SFAT entry at another range of the archive. Each layout's file is
  kept as `make_layout()` makes it for the archives; the archive of the layouts holds a number of copies of each (the
  game has four).

The checks on files the test makes:

- `check_out()`: the shift for pictures such as 5120x1440, 3840x1080, 3840x1600 and 2580x1440 (within 1% of 16:9:
  none).
- `check_layouts()`. `PlayerStatus_00`: the four panes out to their sides, nothing else; moved already: nothing more;
  by another distance: refused, unchanged. `MapMenu_00`: the minimap's panes and the keys at its place; the pause
  map's pane and keys stay. `NumDisplay_00`: both rows to the right; the part's own slide keys stay. `LoadingFade_00`:
  its icon squeezed into the middle 16:9's corner, for the menus' shift alone. A member edges mode does not edit, and
  no distance: nothing. Not a BFFH file.
- `check_refusals()`: each fault on a copy of the layout's file is refused with a why that holds the expected text,
  the file as it was: the pane's name no longer the one the edit looks for (`f_name()`); an animation of
  `PlayerStatus_00` that keys `N_ScaleR_00`'s x (made as a fourth curve of HeartPos); a pane's record offset in the
  pane table pointed at another record or a place inside one; `N_EventEndIn_01` and `N_MiniMap_00` on one record (two
  moves of one value); layouts whose animations differ from the game's in one way each (`MapMenu_00` with another set
  of animations). A moved value that lies on what leads to another field: `N_MiniMapCap_00`'s record laid over
  `N_MiniMap_00`'s so that its parent index is `N_MiniMap_00`'s x (0, the root; a zero whose bits are 0) and its x
  `N_MiniMap_00`'s scale; moving `N_MiniMap_00` makes that parent index a float's bits, which the reading after the
  write finds, and every byte goes back. More panes than a track's index can name. A pane's x keyed by an animation,
  by a curve and by a constant. Every prefix of each layout: refused, never a read past it (the sanitizers watch).
- `check_archive()`: the edit of the archive; again: nothing more, no line. One bad copy of one layout: nothing changes
  in any layout. An archive without one of the layouts. The menus' shift too: `LoadingFade_00`'s two copies, one line
  of its own; the menus' alone: only those. More than 16 copies of a layout. Copies of one layout that share some of
  their bytes, and two layouts that share bytes (a copy of `NumDisplay_00` over `MapMenu_00`'s first). Copies that are
  the same bytes are moved once. One copy moved already, the other not: the line says what the other moved. Nothing at
  out 0. Damaged: a SARC cut short.
- A zstd dictionary is made with a given id, its content and samples taken from a source buffer (size 0 when it
  cannot be made); `Pack/ZsDic.pack.zs` is made as the game's: a SARC of the dictionaries, compressed without one.

The menus over the whole picture (`NX2SARIA_WIDE_MENUS` = full, `check_menus`): the operations of group `G_FULL` in
`wide_hud.c`'s plan, on layouts with the panes, values and animation curves (slopes too) the game's 1.1.1
`PlusMenu_00`, `MenuOverlay_00`, `PaShortCutSlideList_00` and `PaItemMenu_00` hold as far as the edit reads them, with
`MapMenu_00`'s pause map panes and `LoadingFade_00`. No plan uses `OP_MOVE_IN` or `OP_WIDEN`, so they are not reached.

- The fixtures: the page strip's moves, each a curve of both `N_MenuChange` panes with three keys (value, slope);
  another animation that holds a value of the strip the plan moves, and one of the strip's curves with a fourth key;
  turned and scaled panes beside the moved ones, none of them an ancestor of one; the panes the game's animations key
  by constants and curves, none of them moved; a moved pane's x keyed by an animation the plan has no key operation
  for; `N_Arrow_00`'s x, moved, keyed by Loop at two values of the plan's map (two keys at -1850, one at -1858), and
  Loop with one key at -1850, Loop as a constant, Loop with another animation keying the arrow, and no Loop at all;
  `MapMenu_00`'s pause map with one of the panes the menus move keyed by an animation.
- The helpers: a made layout's bytes and where its panes' records and curves' values are; key k's value or slope of
  curve c of animation a (its span is at key -3); a file edited by the shift with a given count of fields changed,
  where a second edit of the edited file (the reading back the game's next start would make of a file served so)
  changes nothing and reports nothing; a copy with one fault that the shift refuses with a why and leaves every byte;
  each pane's field (`AT_X`, `AT_W`, `AT_SX`) holding a value.
- `check_menu_counts()`: the HUD's counts leave the menus' operations of the same layout out (`MapMenu_00`).
- `check_plus_menu()`: u = 660 and F = 1 + 2 * u / 4280 (the pages are 4280 units apart, each 2 * u further from the
  one before). 17 pane fields; per curve the keys at the plan's values, both nonzero slopes and the span:
  (5 + 6 * 4 + 5) * 2. `P_MenuBg_00` stays: the split draws it in pieces. What the plan leaves: the strip's own panes,
  the first page, a grown pane's x, a moved pane's scale. Every key, slope and span of the strip's twelve curves grows
  by one factor: the motion keeps its shape; the y curve of another pane stays, slopes and all. The HUD's shift and
  the menus in the middle 16:9 leave it; another distance over the edited file is refused. 32:9 (u 1920): the strip's
  pages twice as far apart. Refusals, each leaving the file as it was: a pane's x, a width, a key, a span, a curve's
  length, another animation at a value the strip moves (SystemToItem ending where it starts: two keys at -12840).
  Every prefix of what the edit reads (the panes and animations, before the records the split reads): refused, never a
  read past it.
- `check_split()`, the pause menu's backdrop drawn in pieces (`split_layout()`) on `PlusMenu_00` with the records of
  `plus_records()`: the pieces come after `P_MenuBg_00`, every later pane six further on, its parent and subtree end
  with it. The page strip's tracks: `N_MenuChange_00` stays 10, `N_MenuChange_01` moves from 21 to 27, `N_InOut_00`
  stays 9. The parts' panes are past the split, the captures' before it. Each piece is where it moves at u 660, with a
  picture and a material of its own showing its window: the seams the picture's plain stretch from 1880 to 2010 (of
  -320 to 8880), the others their own part of it. The tables of pictures and materials and the per-material array
  grow by six, each new entry `P_MenuBg_00`'s 7; the file's size and the string pool's reach its new end. Split so
  already: nothing to do; the in-place edit of the split file is that of the game's; that file split again: the split
  finds its pieces; another distance does not. 32:9: the gaps three times as wide. The HUD's shift, the squeeze,
  another layout: nothing. What the split refuses, each time making nothing (among them a second picture named
  `P_InfoBg_00` and no list of hashes); every prefix: refused, never a read past it.
- `check_menu_overlay()`: refused for a moved pane inside a scaled or a turned pane (an `OP_MOVE`, not an
  `OP_MOVE_IN`), or keyed by an animation of its own, or not at the game's x.
- `check_slide_list()`: a scissor and the list it clips grow on both sides; their x, their scale and the arrows'
  animation stay. `OP_GROW` looks at the pane's own width alone: a scaled ancestor does not refuse it (the width grows
  in the pane's own space). That is what the edit does today, not a property the plan's layouts need: theirs have
  scale 1.
- `check_item_menu()`: a moved pane whose x an animation keys, with that animation's keys moved with it: both values
  of the map.
- `check_loading_pin()`: the loading icon stretched with its canvas over the whole picture: its width the game's, as
  far from the right side of the grid as the game's at that scale. Pinned, then squeezed: refused, as the pinned icon
  is not at the game's x. Both groups at once (no setting asks for that: `wide_menus` is one or the other): two writes
  to one field. The HUD's shift alone leaves it.
- `check_map_full()`: both groups: the minimap and its keys for the HUD, the pause and world maps for the menus. The
  menus alone: the minimap's panes and the keys of the HUD's key operations as the game has them. The HUD alone: the
  pause map as the game has it. The HUD's edit first, then both: the menus' operations find the game's values but the
  HUD's do not, and `lay_out` takes a layout whole, so the file stays (`wide_hud_archive` keeps the menus' part of
  it). A pause map's pane keyed by an animation: refused for the menus, the HUD's edit of the same layout goes ahead.
- `check_menus_archive()`: `wide_hud_archive` on a buffer the archive may grow in; the archive holds the four HUD
  layouts and the four menus' layouts, one copy each (every other layout of the redesign is missing from it). The
  counts: `PlayerStatus` 4, `MapMenu` 27 + 11, `NumDisplay` 2, `LoadingFade` 2, `PlusMenu` 85 and its 6 new panes,
  `MenuOverlay` 8, the slide list 2, `PaItemMenu` 5. Again: nothing more. A menus' operation of a layout the HUD edits
  too that does not fit: that layout keeps the game's menu layout, its HUD's edit and every other layout go ahead.
  `MapMenu_00` moved for the HUD already, not for the menus: the HUD's part is found moved, the menus' part kept. A
  layout of the menus alone that does not fit: it keeps the game's, the line without the HUD's part. A HUD's layout
  that does not fit: the whole archive as it is. The HUD's part of `MapMenu_00` not fitting: the whole archive as it
  is too; the menus' line written on the way says the HUD's edit goes ahead, which it does not (`wide_hud_archive`
  logs before it knows). When the HUD's part fails too, the archive stays as it is and no line says the HUD's edit
  goes ahead. The menus alone over an archive without the HUD's layouts: the menus' layouts there edited.
- `check_split_archive()`, the split in an archive: four copies of `PlusMenu_00` and one of `MenuOverlay_00`. The
  archive is laid out again around the grown copies, each in a range of its own at its old alignment; what the split
  refuses, or an archive that may not grow, leaves `PlusMenu_00` with the game's menu layout and the archive its size.
  What each copy becomes: the game's file edited in place, then split; `MenuOverlay_00` edited in place. Every entry's
  bytes are in a range of its own after the one before, at its old start's alignment. Again: nothing more. The archive
  as served: the frame records the grown size, the line says so. Two entries of one range: each split copy in a range
  of its own. One copy the split refuses (two texture maps), and an archive that may not grow (no room in the resource
  size table): `PlusMenu_00` keeps the game's menu layout, the archive its size. The rebuild alone: nothing replaced
  gives the archive back; ranges that overlap, or a damaged archive, nothing.
- `check_restbl()`, the resource size table's entry raised for a larger archive (`runtime/hle/restbl.c`
  `restbl_fit()`, `runtime/hle/romfs.c` `restbl_room()`), the entries sorted by their hash: the game's entry, 4175
  bytes over the archive's 9927489, raised by the split's growth keeps the room; a mod's merged entry above the game's
  stays above it; no entry: the estimate for its type; a type the table never lists: nothing.

`game_archive()`, the game's own archive read from `romfs.bin`. Its arguments are `romfs.bin`, the offset and size of
the layout archive, of `Pack/ZsDic.pack.zs` and of the resource size table in it, then (argc 10) the files
`tools/layout_dump.py` `split_picture()` made of the archive's first `PlusMenu_00.buil` at u 640 and 1920. Helpers
count the 4-byte words that differ between two archives and how many of them lie outside the members named, and, for
an archive laid out again after a member grew, the words that differ between the members and the entries that differ
outside the named ones or change size other than `PlusMenu_00.buil`.

- The shapes the settings give (21:9 at 1080 and 1440 lines, 32:9) and windows (3440x1440, 2880x1200, and a 3440x1080
  one wider than 3:1): moved, and only the expected words: four copies of 4 + 27 + 2 fields; MapType's span stays 0.
- The menus' shift with the HUD's (a 3440x1440 picture): the loading icon's x and scale x in its four copies too.
- The menus over the whole picture with the HUD's edges (21:9, 32:9 and a 3440x1440 window): every layout of the
  redesign takes its edit (no "keeps the game's" line), nothing outside them changes, and a second pass on the edited
  archive finds nothing to do. Each copy of `PlusMenu_00` is the game's edited in place, then split.
- After the action prompt's edit of the same archive (`romfs.c` serves it first): both edits, nothing else.
- The split of the game's `PlusMenu_00` at 21:9 and 32:9: byte for byte what `split_picture()` makes.
- The resource size table's entry for the archive: the game's leaves 4175 bytes of room; raised for the archive at
  21:9 it keeps at least that room.

## `tests/wide_hud_projection_check.py`

Checks the full-height exceptions in the 1.1.1 `wide_hud` projection patch.

- The projection match builds v17. The screen lookup then removes the four full-picture vtables from that mask,
  duplicates it across lanes, and only then may the Y row use the vertical squeeze factor.
- After `ldr d16, [x16]` (0xfd400210) x16 holds the canvas or its screen, no longer the patch page: every 128-bit load
  of a page value from there on (LDR immediate with unsigned offset, `w & 0xffc00000 == 0x3dc00000`) reads through
  x17, which the site sets to the page again. Reading the vertical squeeze through x16 (0.20.0-rc.1) took a word of
  the screen object and hid the whole HUD.
- 16:9 and wider ratios supply 1.0 in page +128, so the extra Y path leaves those screens unchanged.

## `tests/wide_hud_title_check.py`

Builds and runs `tests/wide_hud_title_check.c`: `runtime/patches/wide_hud.c`'s second archive, the title screen's
layouts (`UI/Layout2Archive/Title.Product.*`) with `TitleMenuBG_00`'s backdrop over the whole picture
(`NX2SARIA_WIDE_MENUS` = full), on layout files and archives the test makes itself. No game files. The `--help`
description is the string `DESCRIPTION`. With `--romfs DIR`, also on the game's own title archive: the folder holds
`romfs.bin` and its `index.json`, and the program reads the archive and `Pack/ZsDic.pack.zs` from `romfs.bin` at the
offsets the index gives (`game_args()`), edits it for four shapes of picture (21:9, 32:9 and two windows) and prints
the log lines of each. Nothing is written. The program is built with the same sanitizers as
`tests/wide_hud_check.c`; `runtime/simd_fp.h` `FOK` ors two compares on purpose, and clang 18 warns on it.

In the C file the layouts and archives are made from the format as the notes of `wide_hud.c` describe it, with the
panes and widths the game's 1.1.1 `TitleMenuBG_00` holds, so the reader is checked against a writer of its own: a BFFH
layout with panes in the order given (parents first) and an empty table of animations (`pane_at` is where each pane's
record was put); the 1.1.1 layouts of the title archive as far as the edit reads them; a SARC of one other member,
then the members in order; the title archive as the game's holds it: `PaCommonBtnTitle_00`, `TitleMenuBG_00` and
`Title_00`, each a number of times (members 3 * c .. 3 * c + 2 for copy c), with `width_at` the three width fields
within `TitleMenuBG_00` (`C_Capture_00`, `P_Capture_00`, `P_Noise_00`). A shift is the menus' alone (a layout's grid
units wider on each side) or the HUD's alone. What `wide_hud.c` reaches in the runtime is stubbed and every log line
kept. A zstd dictionary is made with a given id, its content and samples taken from a source buffer.

- `check_synthetic()`, with the menus' shift 640 (21:9: 1080 * 2560 / 1080 - 1920): the Common archive's edit leaves
  the Title archive alone; the HUD's edges alone change nothing in the Title archive; the menus' shift changes three
  widths in each of two copies, with one log line; again: nothing to change, no line. An archive that holds a layout
  with another width keeps the game's, and says why; the rest is untouched. A title archive without the layout, or a
  damaged one: the menus keep the game's, nothing else.
- `check_build()`: a shift with the HUD's edges and the loading icon asks nothing of this archive: it is not even
  read.
- `game_archive()`, the game's own archive read from `romfs.bin`: 21:9 at 1080 lines, 32:9 at 1440 lines, a 3440x1440
  window: the menus' shift is `wide_hud_out` of the picture. 16:9: a shift of none edits nothing and reads nothing.

## `tests/widescreen_math_check.py`

Compiles and runs the aspect, scene, heap and Vert+ formulas without game data.

## `tests/worktree_test.py`

`tools/worktree.py` against a scratch repository whose "main checkout" holds dummy ignored data. No game files. It
proves that marker files in the linked directories and in the linked file survive `tools/worktree.py remove`,
`git worktree remove --force` with the junctions still in place, and `git worktree prune` after the worktree directory
is deleted with its junctions in place; and that `git status` in a worktree shows no untracked or typechanged entry
for the links. `make_repo()` links only what exists, as a real checkout may lack some of the data too. `main()` walks
`tools/worktree.py` new / status / remove; an unmerged branch, which `--delete-branch` refuses and `--force` deletes;
the two raw git cases; and it never follows a junction while cleaning up.

## `tests/xlate_package_check.py`

Checks the shader translator guard of the packaging scripts (`tools/build_xlate.py` `source_identity()` and
`check_built()`, used by `tools/make_launcher.py` `check_xlate()`, `tools/make_kit.py` and `tools/make_appimage.py`).
It needs git on `PATH` (`prepare_sources` applies the patches with it) and `tools/vendor`. No game files; any Python
3.10+.

- A folder whose `nx2navi_xlate.id` names this checkout's sources passes; a missing exe or `.id` is refused.
- A folder built before a patch changed is refused, although `identity()` over its own prepared tree equals its `.id`:
  its prepared tree differs from a fresh one by one file, and its `.id` is `identity()` over that tree, as
  `build_xlate.main` wrote it then.
- `build_xlate.py` has no option that writes an `.id` beside an exe it did not build: `--id-only` is gone, the option
  is refused and writes nothing.
- `make_launcher.check_xlate` and `make_kit` stop on a foreign translator, `make_kit` before it downloads or deletes
  anything; `make_launcher` and `make_appimage` run the check before their slow steps. The fresh identity is computed
  once for the packaging scripts; `xlate_smoke.pool_check` is replaced there, since the stand-in exes do not run.
- `tools/xlate_smoke.py` `pool_check()`, which `make_launcher.check_xlate` runs on the exe on Windows:
  `spirv_clamps` reads the array lengths and the UMin clamps of a module made in the test (`spirv()` writes a SPIR-V
  module of the given instructions, each (opcode, operand words...), after a five-word header), `pool_check` refuses
  an exe that does not run, `check_xlate` refuses such an exe beside a matching `.id`, and a translator built here
  (`build/xlate`) passes when there is one.

## `tests/launcher_worker_test.py`

`tests/fixtures/worker_restart_game.c` starts a second copy of its own executable and holds it open until
the test releases it. The native restart test checks that the worker records one restart, retains its
lock, emits no intermediate end and reports the second copy's exit code under the original Play session.
Linux compiles only this stand-in. Windows accepts the compiled fixture through `NX2_TEST_RESTART_GAME`.

`launcher-electron/ui-tests/linux-portable-synthetic.cjs` opens the extracted Electron binary
with a published app and runtime in a temporary installation. It exercises export, import and backup of a scratch save, adds a folder mod,
checks Play and the crash-log marker, and recovers an invalid job marker. It then
opens first run with a generated metadata-only dump. The saved synthetic dump keeps the automatic finder away from the host's drives.

The save export test generates a scratch account and checks the exact ZIP members. It also
tests the diagnostic attachments and redaction, rejection of a relative export directory
and preservation of unrelated preferences. The Linux window test exercises folder choice,
cancellation, the diagnostic toggle and export. Explorer is replaced by a stand-in.

The imports test refuses a running-game simulation and a malformed archive, then injects
a failure between placing the old save in the journal and activating the new one. It checks
that the save and pending import survive that error. A retry imports the waiting archive
and moves it into the existing imported folder. The test also imports one slot and restores
a complete backup. The Linux window test uses the native dialog seams for the exported ZIP
and the generated backup folder. A Node check verifies that a save transaction can finish
after the ordinary request timeout.

The input window test uses native keyboard events and a standard synthetic
controller. It checks focus order and restoration, selection changes, return,
unfocused input suppression, scroll, and overflow at the minimum window size
and at 140% zoom. Hardware Game Mode is a separate check.

The uninstall worker tests schedule the real platform cleanup helper against
two scratch roots, keep or remove the game, and preserve an original dump
inside a candidate path and unrelated files. They exercise a Windows legacy
entry point, verify Linux retains logs, and refuse a running-game simulation,
an interrupted marker, an unsafe game path and a missing entry point. The
Linux window test selects Remove, cancels with Escape, confirms with the
default Keep, and checks the retained save and path-free outcome log.

The locale worker test reads and writes the separate Electron preference,
checks unknown fields and proves the Tk settings file stays byte-identical.
The Node tests check system language fallback, unchanged interpolated player
data, static and dynamic message references, and the same formatting fields
in every language. The locale window test checks the four languages at 720
pixels and 120% zoom, reports a failed write without changing the selection,
checks the native dialog title, and reopens the window with the saved choice.

The appearance test reads a nondefault theme and an old dimming value, compares
the shared palette, changes the custom colors and verifies that invalid choices
leave unrelated settings intact. The Linux window changes the theme, checks a
custom color and its reset, disables animation and changes the sound preference.
The background test uses generated images and verifies rollback after an injected
settings write failure. The window test covers the native picker, cancel, invalid
input, rendered image dimensions, dimming and removing the retained copy. The
Linux window lifecycle test waits for installation controls to be initialized
before asserting the stored hide preference. The asset test substitutes the
game extractors, then reads the fixed font and PCM effect from the real cache.
The window uses a fixture font and generated WAVs to check both cues, volume,
and muting without opening the game's media.

The Steam test uses a scratch account and the real VDF writer. It updates a
legacy shortcut without changing its ID or an unrelated entry, checks image
files and the backup, and substitutes running Steam and the Windows
restart. Linux exercises refusal with Steam open or the wrapper missing,
and detects a Flatpak-only setup. The close test holds the artwork response,
checks that a duplicate request starts one operation and confirms the
worker still owns the lock after EOF. The Linux window verifies detection,
the result of adding the shortcut, and path-free action logs.

The Reports tests use a loopback service. They exercise the first ready screen
with a countdown, the no-countdown question after a job, timeout, deliberate close,
explicit refusal and disconnection. The report preview matches the request bytes
from the Tk reporting core. Both launchers recognize the same sent marker.
The service holds one report while a second synthetic game finishes so the
worker must preserve and then send the newer pending summary separately. A
comment sent with Session Reports Off has no attached session summary, and HTTP
410 records the current version as retired. Tests explicitly set the send switch
off unless they exercise that loopback service. The Linux window tests the
question and preview, consent, the disabled comment control and no service.

The crash offer test installs the active app's existing offer module in a
scratch root. It checks the report question's priority, the input guard, switch
disabled, stale logs, a persistent opt-out, and one-time consumption. It
assembles a preview larger than the worker frame limit, checks path shortening
even when local paths are shown in full, and compares the exact request at a
loopback service. HTTP 400 suppresses future offers in that worker. The Linux
window reads the preview and declines, and the portable test repeats the flow
with the extracted package and published app.

The udmabuf fixture replaces the Linux probe, installed-rule check and
PolicyKit helper. The worker test refuses a stale action and an interrupted
job, honours a declined confirmation, and updates the offer after applying
or removing the fix. The Linux window test captures the native dialog,
checks that cancellation is the default, proves Cancel never reaches the
helper, and simulates a dismissed PolicyKit prompt and a successful retry.

The Discord worker test reads a legacy INI without moving it, refuses to
write during an interrupted job, and changes one value while preserving
CRLF, the configured ID, and the launcher settings. The Linux locale
window switches the preference and compares the game INI and launcher
settings.

The installed settings test changes the update channel, check and install flags,
and cache preference while keeping unrelated keys. Windows also exercises the
low-commit warning setting. An invalid dump leaves the stored choice unchanged;
a metadata-only dump passes the existing validator. The test substitutes the
platform folder opener, verifies both allowed targets and rejects an arbitrary path.
The Linux first-run window test reaches the Settings panel after installing a
scratch game and exercises preference writes, cancellation, dump validation and
logs. The only update request is the one explicitly made by the test.

The Mods worker test adds a folder and ZIP with overlapping synthetic files, checks the
winner after changing the order, crosses a page boundary, and refuses toggles and moves
while a game is detected. Enabling a synthetic code mod sends Play through the existing
rebuild job. The stand-in records the build receipt, and the test checks job cleanup before
Play resumes. The Linux window test uses native dialog seams to import, reorder and toggle
the mods, then checks cancellation, the invalid archive message and path-free action logs.

The manual update check test holds the loopback manifest response while requesting worker
inspection and a duplicate check. It checks that the latter is coalesced and that only the
manifest is fetched. It compares the selected app and staged file before and after. The Linux
window test drives the manual check and displays the release version and bounded plain notes.
The worker bounds a note containing supplementary Unicode characters. The Node test delivers
two individually valid frames in a combined pipe read larger than one frame.

The first-run preference case switches the channel and update check, rejects an invalid
channel and keeps existing fields including telemetry and an unrelated key. On Windows
the stand-in stub records the desktop request. The Linux window test applies the two
common preferences and confirms they persist through installation. It also displays an advisory
missing header. The worker requirements test substitutes the runtime and toolchain probes to
exercise both missing and ready outcomes without touching the settings or game directory.

The first-run destination test checks the default, normalizes a selected nonempty folder and refuses a
relative path without modifying settings or the chosen directory. It also checks the disk-space metadata,
recognizes a completed image and exercises the Windows path-length distinction. `launcher-electron/ui-tests/first-run-synthetic.cjs`
also checks the native dialog boundary, the displayed proposal and cancellation.
The dump test uses `tests/fixtures/worker_install_dump.py` to generate valid header and table metadata
without any game files. The worker distinguishes the expected title from another title. The window
accepts the synthetic dump and preserves its proposal across a rejected selection or cancellation.

Uses temporary launcher roots and child workers to check pre-validation without writes, including malformed settings,
the versioned handshake, inspection, idle EOF, lock refusal and release, the unopenable-lock fallback, sanitized startup
and inspection failures, CLI contention and both Steam behaviors while the worker owns the lock. The Steam test replaces
process creation with a fake. A synthetic staged update is reported without changing its files, malformed frames and
unknown methods are rejected, and repeated action logs are bounded. The cyclic-symlink case is skipped on Windows,
where creating a symlink can require privileges. Fake mod jobs cover cancellation, EOF, progress, successful resumption, memory prompts, failure, a disconnected output and forced worker termination without starting a real game; the interrupted-job marker remains visible to the successor.

On Linux, a fixture with an active release and an AppImage wrapper checks that the worker's opening line in
`launcher.log` names both the AppImage installation and Electron as the process that started the worker.

`tests/fixtures/worker_fake_game.c` can be cross-compiled to a Windows executable and passed as `NX2_TEST_FAKE_GAME` to the worker tests. It waits briefly and writes a marker in its temporary working directory; it does not contain or start the game. The Windows-only test uses the real native spawn and verifies that the stand-in finishes after the worker is killed.

### Packaged launcher and parent-exit checks

`launcher-electron/ui-tests/parent-synthetic.cjs` kills only the Electron main process against a temporary
installation on Linux and Windows. It checks that the idle worker PID disappears and another window obtains the lock.
With a synthetic game held open by a test file, it checks that the same worker and game survive and a second window
reports busy until the fixture ends the game. The Linux case requires real pipe EOF. The Windows case validates the executable path of the worker's parent before killing it because Playwright can
report the PID of a command wrapper. It uses the `tests/fixtures/worker_held_game.c` executable, release Python,
and a packaged Electron app. A separate Python test keeps the protocol writer open while the worker's parent dies
and requires the native parent-exit path to close output and release the lock.

`launcher-electron/ui-tests/installed-runtime-synthetic.cjs` uses an empty folder, a Python 3.11 installation and a
supported release Python to test first install guidance, incompatible runtime guidance, mod rebuild success and
cancellation. Its temporary build tool asserts its file is the copy from the active version and the interpreter is
the one in the selected installation. Its toolchain stamp and files model the toolchain already being present. The
build and game fixtures have no game data and perform no downloads.

The worker window test checks the actual Electron log for build identity, selection, inspection, Play
requests and outcomes, hide and restore, and launcher exit. It checks that the log contains no installation
or executable paths even with the local Python log switch off. The parent-exit test checks that the second
window records lock refusal and its corresponding exit under the same instance ID. The worker window test also
checks that a code-71 exit displays the worker's FATAL detail and logs `ended_with_error`, while an
unexecutable stand-in never starts and logs `could_not_start`. Neither row contains message text.

The synthetic window test holds a stand-in game after spawn and leaves the last game's focus line in place.
It checks that the launcher remains visible with the opening status and Play disabled. It then lets the stand-in
rotate the log and report focus, and checks that the launcher hides and restores after that session.
The same test toggles the shared setting through the home-screen checkbox, reselects the installation
to confirm persistence, and checks window visibility with the setting off and on. It also checks
that the settings write preserves session-report consent and a field unknown to the worker.
It makes the atomic-save temporary path unavailable once, then verifies that the checkbox rolls back
and the settings file is preserved. The worker lock fallback test checks that the preference cannot
be changed when the installation lock could not be opened.

`launcher-electron/ui-tests/published-crash-offer-synthetic.py` drives the worker with a stand-in executable and
the published 0.25.0 and 0.26.0-rc.1 app archives. The newer release receives a marker accepted by its own
`pending()` reader after abnormal exit. Normal exit, opt-out and the older release leave no marker. The worker
finishes and hands off after EOF; a local HTTP test server checks that nothing was sent.

`launcher-electron/ui-tests/published-runtime-synthetic.cjs` tests the packaged worker with the exact app and
runtime archives of 0.24.0-rc.1, 0.25.0 and 0.26.0-rc.1 unpacked in temporary installations. The earlier
Windows fixtures populated `tools/` from the checkout. Those fixtures missed the import failure because
the checkout already exported `shorten_always()`. In addition to inspection and Play with a stand-in
executable, the published-release test verifies the worker and path helper come from their expected locations.
With the local log shortening switch disabled, it checks both the outbound log tail and the crash summary for
shortened paths without sending a request. The worker unit suite also checks boot and privacy when that single
export is absent.

The first-run search test lets the real finder see only a synthetic Downloads folder. It releases
the finder after proving inspection is responsive. It checks the newest proposal, and repeats the
search with an earlier manual selection to prove the manual choice survives. The Linux wrapper
in the Electron window test controls when the same finder can see its synthetic result.


The worker install test drives the request through the same protocol. A stand-in replaces the
expensive build step. Missing choices, insufficient space and an already playable target are
refused. After confirmation on a clear target the test observes successful completion, a removed
job marker and only the two confirmed settings changed. The first-run Electron window test also
cancels the job while it waits, checks marker cleanup, retries, and gets back to a usable Play
control without starting the synthetic game.

The manual rebuild test uses a synthetic existing build. It verifies a job marker blocks
starting, a clean request retains the saved dump, completion follows job cleanup and disabled
cache retention drops the build objects. The first-run Electron test completes a stand-in
rebuild and observes the ready screen and rebuild log without a Play request.

The supervisor test verifies that process identity is available before the start acknowledgement.
It closes the first supervisor's input without starting the command. A second run receives the
acknowledgement, loses its control pipe, and still produces command output and a terminal result
in a separate temporary run directory. It repeats the test with a cancellation flag and a running command.
The worker integration test inspects the mark from inside the stand-in compiler, kills the
worker, checks that a new worker refuses Play while the compiler is alive, and releases the
compiler to finish. Inspection reports a still-live builder and recovers it after it disappears.
The existing Tk marker reader consumes the same extended record. A reused PID and an
unparseable record verify that recovery neither signals an unrelated process nor removes a
record without verification. The Electron window test keeps Play disabled on the latter.
The worker test also requests Wait and observes the final snapshot without sending another
inspection request. A separate test asks the worker to stop a live synthetic build and verifies
the last executable was untouched. The first-run Electron window test drives both recovery
controls against these two states. The explicit recovery case changes the marker during confirmation
and checks refusal. A confirmed unchanged marker and the same marker found after a simulated
reboot exercise both recovery routes. The window test leaves the corrupt marker intact when
the confirmation is cancelled and consumes it when confirmed. The supervisor regression also forces
Windows job assignment to fail and tests that the command still runs by verified PID.
On Linux a stand-in ignores SIGTERM and writes its PID inside the build session. The test
kills the supervisor, confirms that the stand-in is alive, and verifies that Stop waits
for the grace interval before the recovered marker is cleared.


## `tests/launcher_mods_job_characterization_test.py`

Characterizes the Tk adapter's existing handling of a mod rebuild result: success with cache kept or dropped, the
ordered return to Play, and the separate cancelled and failed paths that retain the pending mod names. It also records that Tk can observe a result before `finished` is set, without asserting that the worker should do the same. The test invokes
`App.job_finished` with synthetic jobs and does not run a build or launch a game.

## `tests/launcher_mods_job_core_test.py`

Checks that the worker-facing completion policy keeps a synthetic cache intact while `finished` is unset, then removes
it and allows Play only after the job is fully finished. Tk's existing earlier observation of `result` is separately
characterized and unchanged.

`launcher-electron/ui-tests/linux-portable-synthetic.cjs` launches an extracted Linux test package under Xvfb
against an isolated installation containing a published Linux app and runtime. A synthetic code-71 game
checks runtime selection and the crash-offer marker through the same path as the packaged worker. It also
checks visibility, the renderer's error message and its path-free diagnostic record.
