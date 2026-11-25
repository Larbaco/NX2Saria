# runtime/vk: pipelines, their caches and the shader modules

This page holds the notes of the pipeline files of `runtime/vk/` ([README.md](README.md#the-pages-of-runtimevk) lists
them). The notes of `runtime/vk/vk_pipeline.c` are still comments in it.

The headers below are the rules `vk_pipeline.c` applies, each in a header of its own so that a test checks it without
Vulkan, a driver or files: how `pipeline-cache.bin` is read (`vk_cache_file.h`), the translator stamp of the cache files
(`vk_cache_stamp.h`), the memory gate and the play gate of the background builds (`vk_build_gate.h`,
`vk_play_gate.h`), the library warm's hint (`vk_warm_hint.h`), when the caches are saved again (`vk_save_retry.h`),
what a start owes from an earlier one (`boot_warm.h`) and NVIDIA's shader disk cache folder (`nvidia_cache.h`). The
mesh files decide how the game's task and mesh programs are drawn (`vk_mesh_path.h`) and hold the buffers and records
of the mesh fallback (`vk_mesh_fb.c`, `vk_mesh_fb.h`).

The feature docs: "4. Graphics" in [architecture.md](../architecture.md) (the pipeline cache's two files, the stamp,
the boot warm, the mesh fallback), [performance.md](../performance.md#background-builds-in-play) (the play gate and
what it measures), [building.md](../building.md) (the shader preparation, the library warm, NVIDIA's cache folder and
the checks that drive these files), [logging.md](../logging.md) (the log lines).

## `runtime/vk/vk_cache_file.h`

Reads `pipeline-cache.bin` into memory for `runtime/vk/vk_pipeline.c` `cache_load()`, with 64-bit sizes on every
platform. `ftell` returns a `long`, which is 32 bits on Windows: a file past 2 GB measured as -1 there, and the run
started with an empty cache without a word in the log. The length comes from the 64-bit tell, the read runs in pieces
(`CACHE_FILE_PIECE`, 64 MiB) so that no C library call has to take the file whole, and every refusal names its reason
and the file's size, so `cache_load()` can log it. `tests/pipeline_cache_file_check.py` reads a sparse file just over
2 GB through it.

The reasons: `CACHE_FILE_NONE`, no such file (a first run); `CACHE_FILE_UNSIZED`, the file's length could not be
measured; `CACHE_FILE_SHORT`, not longer than the header every cache starts with; `CACHE_FILE_OVER_CAP`, longer than
the limit, and not read; `CACHE_FILE_NO_MEMORY`, no block of the file's size; `CACHE_FILE_READ_FAIL`, the read ended
early.

### `cache_file_read()`

The whole file in a `malloc` block, or NULL with `*why` set. `*len` is the file's length in bytes whenever it could be
measured, read or not. `min`: a file of at most this many bytes is refused; `cap`: so is one longer than `cap` (0: no
limit), before any of it is read.

## `runtime/vk/vk_cache_stamp.h`

The translator stamp of the pipeline cache files (`runtime/vk/vk_pipeline.c` `cache_stamp_check()`): what a start does
with `pipeline-cache.bin` and `pipeline-cache-play.bin` given the translator that wrote them.
`tests/cache_stamp_check.c` checks it without Vulkan or files.

A pipeline is made from the SPIR-V of a translation, and every translator release makes other SPIR-V, so the entries a
cache holds from an earlier translator are never asked for again once nothing serves that translator's translations any
more. The files only grow (`runtime/vk/vk_pipeline.c` `cache_save_one()`, and the fold in `cache_load()`): without a
stamp a 650 MB file read and held twice at every boot is mostly dead entries. `pipeline-cache.id` names the translator
whose entries the files hold: its base identity (`runtime/gfx/gfx_shader.c` `xlate_identity()`), as in the shader
cache's `identities.txt`. The host options mixed into the shader keys are not part of it, so a change of those alone
keeps the files.

- **The same translator** (`CACHE_STAMP_MATCHES`, `CACHE_STAMP_READ`): the files are read and the stamp stays as it is.
- **Another translator whose translations the shader cache still serves** (`CACHE_STAMP_CARRY`; "Carry" in
  `runtime/gfx/gfx_shader.c`: a shader the current translator has not translated yet uses the previous one's for the
  whole session): the files are read, since the pipelines of those carried shaders are in them, and the stamp keeps
  naming the earlier translator, so the files are dropped at the first start after the carry pass has removed its
  translations.
- **Another translator, not carried, or any other translator in a preparation** (`CACHE_STAMP_DROP`;
  `--prepare-shaders` and the like translate everything under the current translator and carry nothing): the files are
  deleted before they are read and the stamp names the current one.
- **No stamp** (`CACHE_STAMP_NONE`, `CACHE_STAMP_READ_STAMP`; files from before the stamp, none at all, or a stamp file
  that names no identity): the files are read and the stamp then names the current translator.

The cost: what the runs under a carry compiled for the current translator went into the same files, and goes with them
at the deletion. The carry pass usually ends in the first session after an update, so that is what that one session
compiled, and the driver's own cache mostly still has it: the run that deletes the files takes those pipelines from
there and writes `pipeline-cache.bin` again at its exit (`cache_base_at_exit()`), so the start after it reads them from
the file once more. What the driver no longer has is compiled again. A deletion is not a driver change: the start that
follows builds in the background as any other (`runtime/vk/vk_pipeline.c` `bf_decide()`), as it did when the files were
read and none of their pipelines matched the new translator's SPIR-V.

### `cache_stamp_hexval()`, `cache_stamp_parse()`, `cache_stamp_format()`

- `cache_stamp_hexval()`: 1 when `c` is a hex digit, with its value in `*v`.
- `cache_stamp_parse()`: the identity a stamp file's text names (`n` bytes, not terminated): 64 hex digits at its
  start, then nothing but whitespace. 1 and the 32 bytes in `id` when it names one; 0 for any other text (a cut write,
  an empty file).
- `cache_stamp_format()`: the text a stamp file holds for `id`: 64 lowercase hex digits and a line end, 65 bytes,
  terminated (`out[66]`).

### `cache_stamp_state()`, `cache_stamp_action()`

`cache_stamp_state()`: the stamp file's state against the current identity `cur`; `text` NULL means no file.

`cache_stamp_action()`: what the start does. `carried`: the stamped translator's translations are still served (the
shader cache's carry is on and lists it among the previous translators). `prep`: the start is a preparation, which
carries nothing.

### `cache_base_at_exit()`

Whether the run writes `pipeline-cache.bin` at its exit (`P.base_at_exit` in `runtime/vk/vk_pipeline.c`). In play only
the play file is saved, and a pipeline the driver's own cache gives enters the cache of `pipeline-cache.bin` in memory
only, so after a start that finds no `pipeline-cache.bin` (the stamp has just deleted it, or the run that started
without it ended before its exit) every start would ask the driver again for the same pipelines, about 5 ms each
against 0.1 ms from the file (RADV, 2026-11-17), until a fold. Such a run writes the file once, at its exit, with what
it took from the driver's cache and built for that cache: a write at every autosave would copy all of it again each
time, which is what the play file is there to avoid. A run that ends without that save leaves the file missing, and
the next start does the same. `base_size`: the file's size after the stamp's check, -1 when there is none. `prep`: a
preparation, which builds into that file alone and writes it at every save anyway.

### `cache_save_writes_base()`

Whether a save writes `pipeline-cache.bin` (`runtime/vk/vk_pipeline.c` `cache_save()`). `play`: the run has a play
cache (without one everything goes into the one cache, written at every save); `fold`: the play file was merged into it
at the start and is written once; `base_at_exit`: `cache_base_at_exit()`'s answer for this run; `exiting`: the save as
the process ends.

### `cache_stamp_sibling()`

`path` with its ".bin" replaced by `suffix` into `out` (`cap` bytes): `pipeline-cache.bin` gives `pipeline-cache.id`
and `pipeline-cache-play.bin`. A path without ".bin" at its end gets `suffix` after the whole of it. Cut to `cap`.

### `cache_stamp_state_name()`, `cache_stamp_action_name()`

The words the log line gives a state and an action.

## `runtime/vk/vk_build_gate.h`

The memory gate of the background pipeline builds (`runtime/vk/vk_pipeline.c` `gate_enter()`): whether a build may
start, given the system's commit and what the builds already under way were promised. `tests/build_gate_check.c` checks
it without Vulkan.

A pipeline build runs the driver's shader compiler in this process, and what the compiler takes while it works is
committed memory like any other. A gate that only reads the commit in use sees none of it for the builds that have just
started: sixteen builders that each find "enough room" start together and take sixteen shares of the same room. So
every build that starts reserves an estimate of what its compile takes, the gate counts the reservations as if they
were in use already, and a build that does not fit is refused: it does not start, and its caller leaves it to a later
moment or to the draw that first needs the pipeline. A machine at its commit limit fails allocations (Windows) or pages
until the driver gives the device up; a pipeline built later costs one hitch.

`BuildGate`: `free_mb` is the commit headroom in MB that stays free of background builds (0 or less: nothing is
refused); `reserved_mb` the estimates of the builds under way.

### `build_gate_admits()`, `build_gate_enter()`, `build_gate_leave()`

- `build_gate_admits()`: 1 when a build estimated at `est_mb` may start with `used_mb` of `limit_mb` of the system's
  commit in use. A limit below 0 is an unknown one (`runtime/platform/platform.h` `plat_commit_info()`): nothing is
  refused.
- `build_gate_enter()` reserves `est_mb` for a build that starts now; 0 when it is refused, and then nothing is
  reserved.
- `build_gate_leave()`: the build that reserved `est_mb` is over.

## `runtime/vk/vk_play_gate.h`

The play gate of the background pipeline builds (`runtime/vk/vk_pipeline.c` `play_rest()`): whether they rest now, and
the counts the log gives. `tests/play_gate_check.c` checks it without Vulkan.

A background build runs the driver's shader compiler for tens to hundreds of milliseconds, and below-normal or idle
priority does not keep it out of the frame: the game's own calls into the driver wait behind the compile, and the
compiler takes memory the frame needs ([performance.md](../performance.md#background-builds-in-play) measures what it
costs here). So builds made ahead of their first use (the low queue of the pool, the library warm) rest while the frame
shown is a heavy one, and go on in light ones: the title, menus, pause and map screens, loading, a frame not shown for
a while. A pipeline a draw is waiting for never rests.

`PLAY_GATE_SHOWN_MS` (500.0): a frame shown longer ago than this is not a frame in play (a load, a hang).

`PlayGateCount` is what the log's line counts; the caller serializes the calls. `frames`: frames shown while they were
heavy and at least one builder rested. `deferred`: builds held back, each counted once, at the first rest that found it
queued. `rests`: rests begun (a builder that found work and a heavy frame). `rest_ms`: the builders' time at rest,
summed over them. `resting`: builders at rest now.

### `play_gate_rest()`

1 when background builds rest now. `draws_min`: the setting (0: they never rest); `draws_last`: the draws of the last
frame shown, at `shown_ms` (below 0: none shown yet); `light`: the moment is a light one whatever the frame was (an
area load, the boot screen waiting for the warm, a preparation).

### `play_gate_frame()`, `play_gate_rest_begin()`, `play_gate_held()`, `play_gate_rest_end()`, `play_gate_changed()`

- `play_gate_frame()`: a frame was shown; `rest` is `play_gate_rest()` for it.
- `play_gate_rest_begin()`: a builder begins a rest with `held` builds newly held back (those no earlier rest counted).
- `play_gate_held()`: more builds were queued while the builders rest.
- `play_gate_rest_end()`: a builder's rest of `ms` ended.
- `play_gate_changed()`: 1 when the counts moved since `*last`, which then takes them: the log's line comes only when
  there is something new.

## `runtime/vk/vk_warm_hint.h`

What the library warm's log adds when the driver's cache of stage libraries is cold (`runtime/vk/vk_pipeline.c`
`lw_main()`), by GPU vendor, by platform and by why the warm found it cold. `tests/warm_hint_check.c` checks it without
Vulkan.

On NVIDIA under Windows the cache has a size limit, Shader Cache Size in the NVIDIA app. A limit smaller than the
prepared libraries empties the cache as the warm fills it: the driver drops the oldest libraries to make room for the
newest, and the next start's sample finds them gone. Measured on an RTX 5070 Ti (driver 617.42): the game's cache file
stood at exactly 2048 MB, a full warm of 42,122 variants was cold again at the next start (a sample's median 241 ms),
and with Shader Cache Size set to Unlimited the warm finished, the file grew to 2711 MB and later starts found the
libraries in the cache (median 1.1 ms). Elsewhere there is no NVIDIA app and the driver's cache is set otherwise, so the
hints that name the setting are for Windows only.

Why the warm found the cache cold:

- `LW_COLD_FIRST`: cold with no finished warm before it on this driver: a driver update, a change of what the libraries
  are built with, or the build's preparation, which writes no `shader-warm.txt`.
- `LW_COLD_AFTER_WARM`: cold again right after a finished warm on the same driver.
- `LW_COLD_GAVE_UP`: cold after two finished warms in a row: warming is no longer tried on this driver.
- `LW_WARM_FORCED`: `lib_warm = force` on a cache the sample found warm: nothing lost, no cause to name.

### `lw_cold_hint()`

A sentence or two for the end of the log line ("" when there is nothing to add), each starting with a space. `windows`:
the game runs on Windows (`LW_ON_WINDOWS`), where NVIDIA's limit is the NVIDIA app's Shader Cache Size.

## `runtime/vk/vk_save_retry.h`

When the autosave thread saves the pipeline caches (`runtime/vk/vk_pipeline.c` `autosave_once()`), and when it tries
again after a save that did not happen. `tests/save_retry_check.c` checks it without Vulkan.

A save is due when something new entered the caches since the last try. A try that did not save (no copy of the data
for want of memory, a file that could not be written, a build that began between the look and the copy) used to be the
end of it: the next save waited for the next new pipeline, and a run that built none, then ended without its orderly
exit, lost what it had compiled since the last file. Now the try is repeated at the save interval, up to `SAVE_RETRIES`
times in a row, so a folder that stays unwritable costs that many log lines and no more until a new pipeline is built.
The same count bounds the save that is pending from the start (the play file folded into the main one, `pending`),
which before was tried at every interval for as long as it failed.

`SAVE_EVERY_MS` (20000) is the time between two tries, `SAVE_RETRIES` (6) the tries in a row after one that did not
save. `SaveRetry`: `n`, what had entered the caches at the last try; `at_ms`, when that try was made; `failed`, tries in
a row that did not save.

### `save_due()`

1 when a save is to be tried now. `n`: the count of what entered the caches so far (it only grows); `pending`: a save is
owed whatever `n` says.

### `save_tried()`

A try was made at `now_ms` with `n` in the caches; `saved`: the files hold the caches' data now. New content starts a
row of its own. Returns 1 when this was the last try in a row (the caller says so once): the next is made for new
content.

## `runtime/vk/boot_warm.h`

Whether a start does the whole warm on the boot screen (`runtime/vk/vk_pipeline.c` `bf_decide()`, where the reasons
are), and what a start that ended while the boot built pipelines leaves to the next. `tests/boot_warm_check.c` checks it
alone.

### `boot_warm_decide()`

`<cache folder>/boot-warm-owed.txt` is `<GPU and driver> <starts that began the warm>` or `<GPU and driver> done`. The
start that finds the pipeline cache written by another device or driver writes it with 1; a start that finds its own
GPU and driver in it with a count goes on with the warm and counts up; the start whose warm ends writes "done". The
pipeline cache's file keeps the earlier driver's header for some starts after that (what a run compiles goes to the
play cache until that is folded in), and "done" is what keeps those starts ordinary ones. After `BOOT_WARM_TRIES` (3)
starts that began the warm and did not end it the file stays as it is, and starts on that GPU and driver are ordinary
ones too.

- `BOOT_WARM_NO`: nothing owed (a file there is another GPU's or driver's, or not one: the caller deletes it).
  `BOOT_WARM_YES`: this start does the warm. `BOOT_WARM_GAVE_UP` and `BOOT_WARM_DONE`: an ordinary start, and the file
  stays.
- `text`: the file's content, NULL without the file. `key`: this GPU and driver, without spaces. `foreign`: the pipeline
  cache was written by another device or driver. `*tries`: the count to write with `BOOT_WARM_YES`, 0 otherwise. A
  file without a count that is this GPU and driver's is their first start (0).

### `boot_end_decide()`

What a marker means that a start left behind while the boot built pipelines
(`<cache folder>/pipeline-prewarm-running.txt`, `<GPU and driver> <ends in a row before it>`;
`runtime/vk/vk_pipeline.c` `pm_skipped()`). With the crash handler's note the driver crashed in those builds and the
prebuild is skipped on that GPU and driver. Without it the start was closed or stopped, which only counts;
`BOOT_QUIET_ENDS` (3) such ends in a row skip the prebuild as well, for the driver that ends the process without a
fault the handler sees.

- `BOOT_END_OTHER`: the marker is another GPU's or driver's and means nothing here. `BOOT_END_SKIP`: the prebuild is
  skipped on this GPU and driver from now on. `BOOT_END_COUNTED`: this start builds again, and writes `*quiet` into its
  own marker.
- `text`: the marker's content (a marker of 0.9.2 holds the GPU and driver alone). `crashed`: the crash handler's note
  was there. `*quiet`: the ends in a row without a note, this one counted; 0 with `BOOT_END_OTHER`.

## `runtime/vk/nvidia_cache.h`

NVIDIA's shader disk cache folder: what the game puts into the environment before a driver loads
(`runtime/vk/vk_device.c` `nvidia_cache_beside_exe()`), how it measures the folder (`runtime/vk/vk_pipeline.c`
`lw_check()`) and the lines the logs say about both. Plain functions with no driver, environment or file system behind
them, so `tests/nvidia_cache_check.c` checks every rule.

- **Why a folder of its own.** NVIDIA's driver keeps what it compiles in a disk cache of its own. By default that is
  one folder that every program shares, and the limit belongs to the cache: a program that opens it later with a
  smaller limit brings it down to that. The shader preparation (`ocarina --prepare-shaders`) builds about 42,000
  variants into it in one process, some 1.4 to 1.6 GB, so the game keeps the cache in a folder of its own beside the
  exe (`NVC_FOLDER`, `nvidia-cache`), which no other program opens, with a limit of its own (`NVC_SIZE_VALUE`, 8 GiB)
  and without the cleanup. It sets only what the environment does not already set: a player's own folder or limit
  stands.
- **The two systems.** Linux: NVIDIA's kernel driver is loaded (`/proc/driver/nvidia/version`), the folder and the
  8 GiB limit, as the log line has always said. Windows: NVIDIA's user-mode files are installed
  (`System32\nvapi64.dll`), the same folder and limit, and the cleanup off as well. Whether the Windows driver reads
  the variables at all shows in the launcher's log (the folder beside the game fills, or the driver's own folder does).
- **A restart.** A game that restarts itself (a setting that needs it) starts the new process with the environment of
  the old one, variables the game set included, which the new process would take for the player's: its log would say
  so, and an `nvidia_cache_dir = 0` written meanwhile would find the driver already pointed at the folder. So the game
  names the variables it set in one of its own (`NVC_MARK_VAR`), and a start takes those for the game's, sets them
  again or takes them out of the environment. `NVC_MARK_VAR` is `__NX2SARIA_GL_CACHE_SET` and not `NX2SARIA_...`:
  `runtime/platform/platform.h` `plat_getenv()` reads those from the settings.

`NVC_SET_PATH`, `NVC_SET_SIZE` and `NVC_SET_SKIP` are what the game puts into the environment. The verdicts: `NVC_USE`,
set what the `NVC_SET_*` bits say; `NVC_NO_DRIVER`, no NVIDIA driver, nothing is done and nothing logged; `NVC_OFF`,
`nvidia_cache_dir = 0`, the driver keeps its own folder and limit; `NVC_NO_FOLDER`, the folder beside the exe cannot be
used, and the driver keeps its own folder and limit.

`NvcIn`: `windows`, the Windows rules (the cleanup off too); `enabled`, `nvidia_cache_dir`; `driver`, NVIDIA's driver is
installed; `path_set`, `size_set` and `skip_set`, the variables are in the environment and the player's (not the game's
own from before a restart); `folder_ok`, the folder beside the exe is there and takes a new file, read only while
`nvc_need_folder()`. `NvcPlan`: `verdict` is an `NVC_*` value and `set` the `NVC_SET_*` bits, with `NVC_USE`.

### `nvc_need_folder()`

1 when the folder beside the exe has to be made and tested before `nvc_plan()`: the driver is there, the setting is on
and the player has not named a folder.

### `nvc_mark_parse()`, `nvc_stale()`

- `nvc_mark_parse()`: the variables a start inherits that the game set before (`NVC_MARK_VAR`'s text: one digit, the
  `NVC_SET_*` bits); 0 for a text that is anything else.
- `nvc_stale()`: the variables to take out of the environment: the ones the game set before that this start does not
  set again (the setting is off, the driver is gone, the folder cannot be used).

### `nvc_use_line()`, `nvc_stays_line()`, `nvc_refused_line()`, `nvc_off_line()`

- `nvc_use_line()`: the start's line when the game sets variables (`NVC_USE`). `path`, `size` and `skip` are what the
  environment holds after the start; the three sets say which of them are the game's. Linux keeps the words it has
  always logged.
- `nvc_stays_line()`: the start's line when the folder beside the exe cannot be used: `dir`, what is wrong with it ("is
  not writable", "cannot be made", ...) and the system's words for it.
- `nvc_refused_line()`: the start's line when the environment did not take a variable (bits of `NVC_SET_*`): what the
  log would otherwise say the game set is not in force.
- `nvc_off_line()`: the start's line when `nvidia_cache_dir` is 0.

### `nvc_entry()`, `nvc_measure()`

What a folder holds. `NVC_KIND_FILE`, `NVC_KIND_DIR` and `NVC_KIND_LINK` are `PLAT_FS_FILE`, `PLAT_FS_DIR` and
`PLAT_FS_LINK` of `runtime/platform/platform.h`; `NVC_LISTED` and `NVC_MISSING` are its `PLAT_FS_OK` and
`PLAT_FS_NOT_FOUND`. `NvcLister` has the shape of `plat_fs_list()` there: `cb` for each entry of `dir`, and the
listing's error (`NVC_LISTED`, `NVC_MISSING`, other). `NVC_MAX_ENTRIES` (20000) is the number of entries looked at
before the count stops: the folder holds a few files, but a player may name any. `NvcSize`: `unreadable`, the system
refused a folder; `stopped`, `NVC_MAX_ENTRIES` or `NVC_MAX_DEPTH` came first.

- `nvc_entry()`: a link is counted as the file it is, not followed.
- `nvc_measure()`: what lies under `root`: bytes and files (a folder that is not there is 0 and 0). `work`: a buffer of
  `cap` bytes the walk extends and gives back, which holds at least `root` and its longest path.

### `nvc_commas()`, `nvc_size_text()`

- `nvc_commas()`: 12345 as "12,345".
- `nvc_size_text()`: the words after "library warm: " in the game's log: the folder and what it holds, in GB as the
  launcher's log counts (a gibibyte), and what the count missed.

## `runtime/vk/vk_mesh_path.h`

How the game's task and mesh programs are drawn, decided once when the device is made (`runtime/vk/vk_device.c`
`create_device()`). [architecture.md](../architecture.md) describes the mesh fallback itself.

Header only, so that `tests/mesh_path_check.c` checks the decision without a device. The four ways (`MESH_PATH_*`):

- **native**: `VK_EXT_mesh_shader` pipelines.
- **fallback**: the mesh fallback: the programs run as compute passes and an ordinary draw reads what they wrote
  ("mesh fallback" in `runtime/vk/vk_exec_draw.c`, `runtime/vk/vk_mesh_fb.c`).
- **emulation**: the mesh emulation (`mesh_emulation = 1` on a device without mesh shaders,
  `runtime/vk/vk_exec_draw.c`); never on an NVIDIA card (below).
- **none**: a device without mesh shaders with both off: mesh draws are skipped.

### `mesh_radv_task_in()`, `mesh_radv_task_regression()`

Whether a RADV release is one whose task shaders lose the device.

- **The fault.** Mesa 26.2.0 changed where its shader compiler waits for a shader's memory writes, and a task shader
  since then can mark its record as ready before the payload and the mesh workgroup counts are written. The mesh stage
  then reads what is not there yet and the GPU stops answering: the kernel resets it and the game ends with "the GPU
  device was lost" within seconds of the first mesh draws. A session report shows it on an RX 6800S with Mesa 26.2.7
  after 17 seconds. Mesa's own tracker has it on RX 6000 cards and on the integrated GPUs of the same generation.
- **The range.** `mesh_radv_task_series` has one row for each release series with the fault, and the patch number of
  the first release of that series with the fix, or `MESH_RADV_NO_FIX` while none is out. Today both rows are open:
  26.2.7 and 26.3.1 are the newest releases and neither has the fix, which is on Mesa's main branch since 2026-11-19 and
  queued for both series. A row gets its number only from a published release that was checked to hold the fix; until
  then every later release of the series is drawn for through the fallback too, the fixed one included.
- **Outside it.** Every release before 26.2.0, and every version that is not of a listed series: a later series
  branches from a main branch that has the fix. A snapshot of the main branch says 27.0.0, and some packagers number
  theirs 26.99.99; both are outside. A snapshot built between 2026-10-16, when the main branch became 27.0.0, and the
  fix is not told apart from a fixed one and draws natively: `mesh_fallback = 1` is what is left for it.
- **Which cards.** Every RADV device with mesh shaders in those releases, not only the generation the reports are from.
  The fault is in the code every task shader goes through, with no branch on the hardware generation, so nothing in it
  says that newer cards are safe; the reports name RX 6000 cards only.
- **The version.** `driverVersion` of the device's properties, which Mesa fills as major, minor and patch in the layout
  of `VK_MAKE_VERSION`. The driver is told by its `VkDriverId` (`MESH_DRIVER_RADV` is `VK_DRIVER_ID_MESA_RADV`): another
  driver with the same number is not in the range. `mesh_radv_task_in()` takes the table as an argument so that the
  test can give it a series with a fixed release.

### `mesh_path_decide()`

`has_mesh`: the device has `VK_EXT_mesh_shader` with task and mesh shaders; `amd_sc`: AMD's Windows driver for RDNA 1
and 2; `vendor`: the device's `vendorID`; `fallback`: the `mesh_fallback` setting ("auto", "0", "1"; anything else
reads as auto); `emulation`: `mesh_emulation` ("0" off). `*why`: the reason, for the boot line.

- **AMD's Windows driver for RDNA 1 and 2.** `mesh_fallback = auto` (the default) takes the fallback on a device
  without mesh shaders, and on AMD's Windows driver for RDNA 1 and 2 (`vk_caps.amd_sc`), which has them on RDNA 2. That
  driver lost the device ("No fault detected", a timeout) minutes into play on RX 6600 to RX 6950 XT cards in every
  version from 0.6.0 to 0.9.2 (0.10.x built its mesh pipelines without the driver's optimiser, `NX2SARIA_VK_NOOPT`,
  which no reporter has confirmed), with the pipeline library, the replay workers, the asynchronous replay, the lazy
  write-back and the low-end GPU mode each on and off. Where the pass marks named the pass the GPU was in, it was one
  that starts with a native mesh draw. The same driver on RDNA 1 has no mesh shaders and draws through this fallback,
  and AMD's driver for RDNA 3 and 4 (LLPC) draws them natively; neither has such a report. That the native mesh path is
  the cause is not shown: the fallback is what the marks leave to try, at the cost of a compute pass and a pass break
  per mesh draw, and of mesh programs that are left out of the picture until their compute passes are built, at their
  first sight in a run. `mesh_fallback = 0` draws them natively again; 1 takes the fallback on every device.
- **Intel.** auto takes the fallback on Intel's cards with mesh shaders too (vendor 0x8086, Arc and the integrated GPUs
  that have them), on every Intel driver. On an Arc B390 (integrated) and an Arc B580 with Intel's Windows driver
  32.0.101.9150 the fog over Kokiri Forest's ground showed small holes the grass shows through (issue #13): the fog's
  fragment programs drop a pixel whose depth is past the depth its own depth pass wrote, and there the two differ.
  0.18.2 pinned the task stage to 32 lanes and made the mesh programs' positions invariant; the player saw the holes
  with both, and they went only with `mesh_fallback = 1`. Neither change was a fix of a shader compiler's, so what is
  left is how the native mesh path writes and compares depth on this hardware, which Mesa's Intel driver (ANV) shares:
  there it is taken as a precaution, with no report from it either way. `mesh_fallback = 0` draws them natively again.
- **NVIDIA and the emulation.** The mesh emulation is the older of the two ways for a card without mesh shaders, and
  the low-end GPU mode turned it on up to 0.10.1 (`runtime/cfg/cfg.c` `lowend_set()`). On NVIDIA's cards without mesh
  shaders (GTX 10 and older) it ended the game inside the driver at the first mesh draws of an area: an access
  violation at `nvoglv64.dll+0x10cdffe` on a GTX 1050 Ti and on a GTX 1070, both on 0.10.1, and a Titan X (Pascal)
  report of the same mode. The GTX 1050 Ti ran on once `mesh_emulation` was set to 0, through the fallback, with the
  rest of that mode still on. So an NVIDIA card never takes the emulation: with it asked for, the card is drawn for as
  if it were not.

`radv_task_bug` is `mesh_radv_task_regression()`'s answer. It is read after the settings and after the cases that hold
on any driver, so `mesh_fallback = 1` and `mesh_fallback = 0` act as on every device, and a card without mesh shaders
is drawn for as before. With `mesh_fallback = auto` it takes the fallback, and `mesh_path_fallback_default()` counts it
as the device's default.

The log says what was read and what followed from it (`create_device()`, on RADV with mesh shaders only):

- `vk: mesh path: RADV <version> is inside|outside the releases whose task shaders lose the device (...): mesh_fallback
  = auto takes the mesh fallback|draws mesh programs natively`. Inside, it also says whether the series has a fixed
  release.
- `vk: mesh path: WARNING: mesh_fallback = 0 draws mesh programs natively on this RADV release all the same: the device
  can be lost within seconds`, when the player's setting forces the native path inside the range.
- The `vk: mesh fallback:` line that follows gives the reason as for the other defaults.

`tests/mesh_path_check.py` checks the range release by release and the decision.

### `mesh_path_fallback_default()`

1 when `path` is the fallback and `mesh_fallback = auto` would choose it on this device too (AMD's Windows driver for
RDNA 1 and 2, Intel, a device without mesh shaders): the fallback is the device's default, whether or not
`mesh_fallback = 1` also asks for it; 0 where `mesh_fallback = 1` forces it on a device that draws mesh programs
natively. `NX2SARIA_MESH_FALLBACK_BATCH = auto` batches where this is 1.

## `runtime/vk/vk_mesh_fb.c`

The mesh fallback's buffers and records. A task or mesh program runs as a compute dispatch that writes a storage buffer
(`runtime/gfx/mesh_fb.c` makes the modules), and the draw reads it ("mesh fallback" in `runtime/vk/vk_exec_draw.c`):

- **task buffer**: a task workgroup's payload at its index, payload-size apart.
- **list**: per mesh workgroup after a task stage: its task and its index among that task's
  (`runtime/shaders/meshfb_scan.comp`).
- **mesh buffer**: a mesh workgroup's vertex lines and primitive area at its slot.
- **records**: per draw 64 bytes (`REC_BYTES`): the indirect arguments of its task dispatch, its mesh dispatch and its
  draw.

The three buffers are reused by every draw: a draw's dispatches write them and its vertex stage has read them before
the next draw's dispatches run, with a barrier in between (`runtime/vk/vk_exec_draw.c` `mfb_draw()`), so their size is
what the largest draw needs, not what a frame's draws need together. `mesh_fallback_mb` sets it: an eighth for the task
buffer, the rest for the mesh buffer. Workgroups past either buffer's end write nothing and draw nothing (the modules
check the buffer's length); the draws that happened to are counted and the log names the size they needed. A record is
used by one draw and written again only after `MESHFB_RECS` (4096) further draws.

The state `M`:

- `task_bytes`, `mesh_bytes`, `list_bytes`: the buffers as one storage binding spans them.
- `stat_map`: per record, the mesh workgroups and the task workgroups it asked for.
- `cap_slots`, `slot_words`, `cap_tasks`: what the record's draw had room for.
- `fence`: the submission the record's note is in (`vk_meshfb_note()`).
- `checked`: the records read back so far (`vk_meshfb_poll()`), as `M.next` counts them.
- `owner`: the record's program's history (`MfbCs.hist` in `runtime/vk/vk_exec_draw.c`), NULL: none.
- `batched`: the record's draw had a part of the buffers (a batch).
- `part_on`: the draw being recorded uses `part` (`vk_meshfb_part_take()`).
- `used`: the parts taken for the pre-pass command buffer being recorded.
- `batch_short`: batched draws that asked for more than their part held.
- `readmits`: programs taken back into batches after such a draw (`runtime/gfx/mfb_batch.h` `mfb_hist_note()`).
- `closed`: a draw went one by one since the parts began: the next batched cuts (`runtime/gfx/mfb_batch.h`).
- `peak_mesh`: the most of the mesh buffer the parts of one pre-pass command buffer took.

### `vk_meshfb_init()`

Creates the buffers and the scan pipeline on first use. 0 when that failed (logged once): the draws are skipped. The
list buffer has an entry per slot of 512 bytes (`mesh / 128`), and no mesh program's slot is smaller. The clamp of a
count the GPU wrote (`vk_meshfb_clamp()`) is a second pipeline of the same layout: binding 0 and three push constants.

### `vk_meshfb_buffer()`, `vk_meshfb_mesh_words()`

`vk_meshfb_buffer()`: the buffer a module's extra storage binding reads or writes (`MESHFB_CB_*` of
`runtime/gfx/mesh_fb.h`), whole or as much of it as one storage binding may span; for a batched draw, its part
(`vk_meshfb_part_take()`). `vk_meshfb_mesh_words()`: the mesh buffer's size as a binding sees it, in words.

### `vk_meshfb_part_take()`, `vk_meshfb_part_done()`, `vk_meshfb_part_reset()`, `vk_meshfb_part_first()`

A batch's parts (`runtime/vk/vk_exec_draw.c` `mfb_draw()`): each batched draw of a pre-pass command buffer takes a part
of the three buffers from the start, in turn; the buffers' use starts over with the next pre-pass command buffer, which
begins with a full barrier (the draws of the part before may still read them).

- `vk_meshfb_part_take()`: the next part of the buffers, of these many bytes each, for the draw about to be recorded;
  until `vk_meshfb_part_done()`, `vk_meshfb_buffer()`, the record, the clamp and the scan use it. 0: no room left.
- `vk_meshfb_part_reset()`: at the end of a pre-pass command buffer (`runtime/vk/vk_sync.c` `pre_end()`) the parts
  start over.
- `vk_meshfb_part_first()`: no part taken since the last reset.

### `vk_meshfb_one_by_one()`, `vk_meshfb_closed()`

`vk_meshfb_one_by_one()`: a draw is recorded one by one; with parts taken, the next batched draw cuts first.
`vk_meshfb_closed()`: a draw went one by one since the parts began, so the next batched cuts.

### `vk_meshfb_full_barrier()`, `vk_meshfb_pre_end()`

- `vk_meshfb_full_barrier()`: a full memory barrier in a command buffer that is not the part's: every write before it
  visible to every read and write after it, in `cb` (a pre-pass command buffer: `vk_exec_barrier` is the part's, with
  its pass and layouts).
- `vk_meshfb_pre_end()` (`runtime/vk/vk_sync.c` `pre_end()`): the pre-pass command buffer ends. With parts taken, a
  full barrier first, so the part's draws see what the dispatches wrote; then the parts start over.

### `vk_meshfb_batch_short()`, `vk_meshfb_batch_readmits()`, `vk_meshfb_batch_peak()`

- `vk_meshfb_batch_short()`: batched draws that asked for more than their part held, so far.
- `vk_meshfb_batch_readmits()`: programs taken back into batches after a draw short of its part.
- `vk_meshfb_batch_peak()`: the most of the mesh buffer (`*cap` bytes) one pre-pass's parts took.

### `vk_meshfb_caps()`, `vk_meshfb_list_bytes()`, `vk_meshfb_slot_room()`

- `vk_meshfb_caps()`: the task and mesh buffers' bytes. `vk_meshfb_list_bytes()`: the list buffer's bytes.
- `vk_meshfb_slot_room()`: the mesh slots a one-by-one draw has room for: the whole buffers' room.

### `record_check()`

The record about to be written again: what its last draw asked for against what it had room for. The stat buffer is
`HOST_ACCESS_RANDOM`, which may not be coherent, so its allocation is invalidated before the read. A batched draw short
of its part is logged by the batch's own line and not again as a short draw. A draw where only the task count was past
the task buffer is a count the GPU wrote, kept by `runtime/shaders/meshfb_clamp.comp` before it cut the dispatch (until
2026-11-22 such a draw was not seen); the mesh workgroups its tasks started all fit.

### `vk_meshfb_record()`

A draw's record, 64 bytes of the record buffer (`*rec`) at the offset returned: the task dispatch at +0, the mesh
dispatch at +16, the draw (`VkDrawIndirectCommand`) at +32. Records the fills and copies that start it, outside a pass:
the workgroup count from 4 bytes at `src_off` of `src`, or `count` when `src` is `VK_NULL_HANDLE`. Without a task stage
that count is the mesh dispatch and the draw's instance count; with one it is the task dispatch, and `vk_meshfb_scan()`
writes the rest. `slot_words`: of the draw's mesh program, for the report of a draw the mesh buffer was too small for.
`owner`: the program's history (`MfbHist`), which the record's read back updates; NULL: none. Returns the number of
commands recorded in `*cmds`.

- When the ring comes round to records the poll had not reached, `M.checked` moves up with it.
- `cap_tasks` starts at 0xffffffff: `vk_meshfb_scan()` knows the payload size.
- **Every word of the record is written by exactly one command.** A fill of the whole record followed by fills and
  copies into parts of it were transfer writes to the same bytes without a barrier between them, which the GPU may run
  in any order: on an RX 580 the vertex count, the first vertex or instance or the task count was the fill's 1 in about
  half the draws, and the foliage of the mesh programs flashed (missing, or white from the wrong slots). Fills and
  copies only: they go through the recorder thread (`runtime/vk/vk_rec.h`) like the replay's other commands.
- `copied` is the words the GPU-written count goes to. The workgroup count goes to the task dispatch, or to the mesh
  dispatch, the draw's instances and the report (`to`).

### `vk_meshfb_scan()`

The pass between the task and the mesh dispatch (`runtime/shaders/meshfb_scan.comp`): binds its own compute pipeline
and set. `clamped`: `vk_meshfb_clamp()` ran for this record and wrote its report of the task workgroups asked for
(word 13).

### `vk_meshfb_clamp()`

For a record whose count came from `src` (a count the GPU wrote, copied into record `at` by `vk_meshfb_record()` with a
source): cuts it to what the buffers hold before a dispatch reads it, in the task dispatch (`task_words`, the task
stage's payload words) or without a task stage (0) in the mesh dispatch and the instances
(`runtime/shaders/meshfb_clamp.comp` says why). Binds its own compute pipeline and set; four commands, between two
barriers. One single-thread dispatch; the caller orders it after the record's copies and before the first dispatch.

### `vk_meshfb_note()`

After the record is complete (the copies or the scan, and a barrier): keeps what the draw asked for where the host
reads it later. One copy command. `fence`: the submission's value.

### `vk_meshfb_poll()`

The records whose submissions the GPU finished (`done`: `vk_sync_fence_done`), read back in order: the programs'
histories (`runtime/gfx/mfb_batch.h`) grow, and a batched draw short of its part excludes its program, within frames of
the draw and not 4096 records later, when the record is written again.

### `vk_meshfb_short()`, `vk_meshfb_shutdown()`

`vk_meshfb_short()`: draws so far whose programs emitted more than the buffers hold, the records not looked at yet
included: call it with the device idle (a probe, the shutdown). `vk_meshfb_shutdown()`: the device must be idle.

## `runtime/vk/vk_mesh_fb.h`

The declarations of the mesh fallback's buffers and its records (`runtime/vk/vk_mesh_fb.c`, where their notes are); the
modules are `runtime/gfx/mesh_fb.c`'s, the draws `runtime/vk/vk_exec_draw.c`'s ("mesh fallback"). Two of its functions
are defined in `vk_exec_draw.c`:

### `vk_exec_mfb_batch_start()`, `vk_exec_mfb_batch_exit_log()`

- `vk_exec_mfb_batch_start()` reads and logs `NX2SARIA_MESH_FALLBACK_BATCH`. `fallback`: whether mesh programs go
  through the fallback on this device; `fallback_default`: whether it is the device's default
  (`runtime/vk/vk_mesh_path.h` `mesh_path_fallback_default()`); `path_name`: how mesh programs are drawn here, for the
  line of a device where the batch is inactive.
- `vk_exec_mfb_batch_exit_log()`: the batch's totals, with it on (`runtime/gfx/gfx_present.c` `rt_exit_steps()`).
