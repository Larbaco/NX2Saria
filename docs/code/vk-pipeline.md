# runtime/vk: pipelines, their caches and the shader modules

This page holds the notes of the pipeline files of `runtime/vk/` ([README.md](README.md#the-pages-of-runtimevk) lists
them).

`vk_pipeline.c` maps the state the replay collects to Vulkan pipelines and descriptor layouts, rewrites the translator's
SPIR-V where Vulkan or a driver needs it, and keeps what was built across runs: the pipeline cache's two files, the
pipeline keys, the stage libraries and their warm, the compute pipelines the boot keeps, and the background builds with
their gates. Its first line is a licence notice that stays in the code; the rows of `THIRD_PARTY_NOTICES.md` say what it
covers. The headers below it are the rules `vk_pipeline.c` applies, each in a header of its own so that a test checks it
without Vulkan, a driver or files: how `pipeline-cache.bin` is read (`vk_cache_file.h`), the translator stamp of the
cache files (`vk_cache_stamp.h`), the memory gate and the play gate of the background builds (`vk_build_gate.h`,
`vk_play_gate.h`), the library warm's hint (`vk_warm_hint.h`), when the caches are saved again (`vk_save_retry.h`), what
a start owes from an earlier one (`boot_warm.h`) and NVIDIA's shader disk cache folder (`nvidia_cache.h`). The mesh
files decide how the game's task and mesh programs are drawn (`vk_mesh_path.h`) and hold the buffers and records of the
mesh fallback (`vk_mesh_fb.c`, `vk_mesh_fb.h`).

The feature docs: "4. Graphics" in [architecture.md](../architecture.md) (the pipeline cache's two files, the stamp,
the boot warm, the mesh fallback), [performance.md](../performance.md#background-builds-in-play) (the play gate and
what it measures), [building.md](../building.md) (the shader preparation, the library warm, NVIDIA's cache folder and
the checks that drive these files), [logging.md](../logging.md) (the log lines).

## `runtime/vk/vk_pipeline.c`

Graphics and compute pipelines and descriptor layouts of the Vulkan replay. The op-stream replay
(`runtime/vk/vk_exec.c`) collects NVN state into a `VkPipeState` and asks for the pipeline of each draw; this file maps
that state to Vulkan, numbers the descriptor bindings and keeps the caches.

**Binding model.** The translator (`tools/xlate/Program.cs`, a SPIR-V back end) decorates every resource of a shader
with DescriptorSet 0 and numbers the bindings of each shader from 1 in the order it meets them (Accessor
`Create*Binding`); binding 0 is its support buffer (ResourceManager: `SupportBuffer.Binding`, a std140 uniform buffer
every shader declares). The numbering is per shader, so the vertex and fragment stages of one program collide. The
scheme here:

- One descriptor set, set 0, per pipeline; no push constants (the translator's SPIR-V uses none);
- Binding 0: the support buffer, one uniform buffer shared by all stages of the pipeline;
- Vertex stage: its own bindings 1..n unchanged; fragment stage: its bindings b >= 1 move to b + n (the SPIR-V Binding
  decorations are rewritten when the module is created); compute: its own bindings;
- Descriptor types from the reflection: constant buffers `UNIFORM_BUFFER`, storage buffers `STORAGE_BUFFER`, textures
  `COMBINED_IMAGE_SAMPLER` (`UNIFORM_TEXEL_BUFFER` for buffer textures, `SAMPLED_IMAGE` for separate ones), images
  `STORAGE_IMAGE` (`STORAGE_TEXEL_BUFFER` for buffer images), array lengths as descriptor counts;
- Set 0 is a push-descriptor set (`VK_KHR_push_descriptor`) when its descriptors fit `maxPushDescriptors` (32 on the RX
  9070; a pipeline layout may hold one push set only, which rules out a set per stage); larger ones (a vertex and a
  fragment stage near the census maxima of 15 and 27) are ordinary sets the replay allocates (`VkPipeInfo.push` 0).

A fixed layout per stage combination (a binding per constant buffer, storage buffer, texture and image slot per stage)
would hold about 150 descriptors and could never be pushed, so layouts are made per distinct binding list and shared by
every pipeline with that list; `VkPipeInfo.bindings` tells the replay which NVN slot feeds each binding.

Fragment inputs the vertex stage does not write (stages are translated separately) get outputs added to the vertex
module, as Vulkan rejects such pairs, initialised to (0, 0, 0, 1) as Maxwell reads an unwritten output
([maxwell.md](../maxwell.md) 1.2). The unwritten components of an output the vertex stage does write start at (0, 0, 0,
1) in the translator (`tools/xlate/patches/0006-initialise-unlinked-vertex-outputs.patch`).

**Pipeline key.** Everything Vulkan 1.3 can take as dynamic state is dynamic: viewports and scissors with count, depth
bias, blend constants, stencil masks and references, cull mode, front face, topology (within its class), depth and
stencil tests and ops, depth bias enable, rasterizer discard; vertex input (`VK_EXT_vertex_input_dynamic_state`, else
binding strides only); and, where `VK_EXT_extended_dynamic_state3` offers them, depth clamp, polygon mode, blend
enables, equations, write masks and alpha to coverage. The key holds the rest: shaders, target formats, sample count,
topology class, depth clip mode, logic op and what is not dynamic on the GPU. The `VkPipelineCache` persists to
`pipeline-cache.bin` next to `nx2saria.exe` (`NX2SARIA_PIPELINE_CACHE` overrides the path).

The vertex format table has a 3-component fallback. The pipeline key holds the program, target formats, blend and write
masks, vertex layout and sample count. The binding conventions are those of the shader translator.

### Device capabilities and the pipeline cache

Set up on first use, under `P.lock`.

`NP_16`, `NP_32`, `NP_64`: the float widths in `P.nan_preserve` and `spirv_nan_preserve()`.

`P`:

- `int indep`: stage-independent layouts (`vk_caps.gpl`): vertex set 0, fragment set 1.
- `int push_stage`: `GFX_STAGE_*` whose set is pushed with independent layouts.
- `int gpl`: draws whose pipeline is building get a fast-linked one (`vk_caps.gpl`).
- `int mesh_lib`: mesh pipelines are linked from stage libraries too (`mesh_lib_init()`, `mesh_libs()`).
- `P.g`, the mesh pipelines' share of its counts: `m_links` fast links and their link time; of the `ready_mesh`
  pipelines a draw missed, `m_ready_fast` became usable through a fast link, `m_ready_ms` after the miss in all;
  `m_lib_*`: [0] their task and mesh libraries, [1] their fragment shader libraries (`lib_build()`).
- `int vs_outs_all`: `NX2SARIA_VS_OUTS`: 0 union of paired fragment inputs, 1 (all) all 32 output locations.
- `int wide_lines`: the device draws lines of other widths than 1 (`vk_pipeline_line_width()`).
- `float line_w[2], line_gran`: its range of widths and their step.
- `int cache_control`: `pipelineCreationCacheControl`: a create can ask for the pipeline caches only.
- `int feedback`: `NX2SARIA_PIPE_FEEDBACK`: 1 count pipeline cache hits, 2 also log each compute and graphics build.
- `uint64_t cache_loaded`: bytes of `pipeline-cache.bin` the driver took at boot.
- `VkPipelineCache play`: what this run compiles (`pipeline-cache-play.bin`); `VK_NULL_HANDLE`: all goes into
  `P.cache`.
- `uint64_t play_loaded`: bytes of `pipeline-cache-play.bin` the driver took at boot.
- `uint64_t saved[2]`: the data size of cache [0] and play [1] that their files hold; 0: not known.
- `volatile uint64_t held[2]`: the data size of cache [0] and play [1] when last asked (`cache_save_one()`,
  `cache_sizes_ask()`); 0: not asked yet (the memory report takes the loaded size).
- `int fold`: the play file's content was merged into `P.cache`: saving `P.cache` removes the play file.
- `int base_at_exit`: the start found no `pipeline-cache.bin`: the save at exit writes it (`cache_load()`).
- `volatile uint64_t play_made`: pipelines this run compiled into play.
- `int pv_last`: `provokingVertexLast` enabled: NVN's default is the last vertex.
- `VkShaderStageFlags sg32_stages`: stages that accept `requiredSubgroupSize` = 32 (0: not available).
- `int reconverge`: modules with subgroup ops get MaximallyReconvergesKHR (`spirv_reconverge()`).
- `uint32_t nan_preserve`: float widths that get SignedZeroInfNanPreserve (`spirv_nan_preserve()`):
  `NP_16`|`NP_32`|`NP_64`.
- `int nan_bits`: NaN tests as integer bit tests (`spirv_nan_bits()`).
- `volatile uint32_t nan_bits_tests, nan_bits_selects, nan_bits_modules`: rewritten so far (the first log line).
- `int mesh_invariant`: mesh modules' position gets Invariant (`spirv_mesh_invariant()`).
- `volatile uint32_t mesh_invariant_modules`: so far.
- `int mesh_sg`: task stages pinned to 32 and task and mesh stages run full subgroups (`stage_sg()`).
- `volatile uint32_t sg_task_pinned, sg_task_full, sg_mesh_full`: stage create infos given them so far.
- `int tpl`: descriptor update templates on (`make_tpl()`).
- `char play_path[PLAT_PATH_MAX * 2 + 8]`: `pipeline-cache-play.bin` beside `cache_path`.

`Layout`:

- `VkDescriptorSetLayout set1`: independent layouts: the fragment set (set 1); `VK_NULL_HANDLE` otherwise.
- `int push1`: set 1 is a push-descriptor set.
- `uint32_t n0`: independent layouts: b[0..n0) are set 0, the rest set 1.
- `VkPipelineLayout half[2]`: GPL libraries: {set 0, null}, {null, set 1} (made on first use).
- `VkDescriptorUpdateTemplate tpl[2]`: per set (`make_tpl()`); `VK_NULL_HANDLE`: written with write structs.
- `uint8_t emu`: `NX2SARIA_MESH_EMULATION`: a mesh layout whose task and mesh stages run as compute.
- `VkDescriptorUpdateTemplate tpl_c`: emu: set 0's template for the compute bind point.

`Pipe`:

- `int building`: a background job owns `info.pipeline` (under `P.lock`).
- `struct Job *job`: that job while building.
- `const struct Layout *lay`: graphics: the layout it was entered with.
- `VkPipeline fast`: GPL: the fast-linked pipeline, until the optimised one replaces it (`fast_retire()`).
- `uint8_t link_queued`: a waiting draw queued its fast link to the workers (`LibJob.p`), not yet run.
- `uint8_t used`: compute: a dispatch of this run asked for it (`vk_pipeline_compute()`).
- `uint8_t pre`: compute: built and kept by the boot prebuild (`lb_compute()`) or `bg_main()`.
- `uint8_t optimised`: graphics: its pipeline is the whole build that replaced a fast link (`fast_retire()`).
- `uint32_t used_min`: `pipe_minute` when a draw or dispatch last looked it up; 0 none in this run.
- `uint64_t t_miss`: graphics: when a draw first missed it (ns), 0 once it is usable (`ready_note()`).
- `uint64_t rel_fence`: taken out by `pressure_release()`: destroyed once this value is done (`release_drain()`).

### `minute_now()`

The memory report's clock for pipelines not used lately (`vk_pipeline_mem()`): wall minutes, plus one so that 0 stays
"never". The autosave thread moves it every half second; a lookup stores it into its Pipe under `P.lock`, which the
lookup holds anyway, so the per-draw cost is one compare. A pipeline that only the replay's memo of the previous draw
served (`PM` in `runtime/vk/vk_exec_draw.c`) keeps an older minute: at most the one pipeline each replay context
remembers.

### `key_hash()`

Hash of a pipeline table key, 8 bytes per step: a draw hashes its whole GKey, and byte-wise FNV over it cost more than
the lookup. Only the in-memory table uses it; the key file keeps `fnv()`.

### `cache_path_init()`

The cache file lives next to `nx2saria.exe`, so every build dir (the main build and each private fork) has its own:
forks that shared build\v121's cache broke each other's runs (2026-10-22).

### Pipeline binaries

`VK_KHR_pipeline_binary`; `NX2SARIA_PIPE_BINARIES`, default 0 while it is being tested; 1 on.

`pipeline-cache.bin` does not carry compiled pipelines from one executable to another on NVIDIA 621.01: two
byte-identical exes under different names, every compute module hash-identical, and the second compiled 317 of the 323
compute pipelines the first had read from the file (2026-11-09). A pipeline binary made from one exe's pipeline creates
the same pipeline in another in 0.5 ms instead of 26 ms. So where the driver has the extension, every pipeline, stage
library and compute pipeline is created through `pipe_create()`: its driver pipeline key (`vkGetPipelineKeyKHR`,
computed from the create info) is looked up in `pipeline-binaries.bin` next to `nx2saria.exe` and the pipeline is made
from the stored binaries; a key not there is compiled with its data captured, and its binaries are appended to the file
(autosave, every 2 s). `pipeline-cache.bin` is then neither read nor written: the driver's own disk cache serves an
unchanged exe, and the binaries serve a renamed or updated one.

Each binary is stored once under its own binary key: on NVIDIA 621.01 nearly every graphics pipeline has, besides its
own binaries of 20-200 KB, one 2 MiB binary that is byte-identical across pipelines (2,506 copies in a 4.7 GB
first-version file).

File: header {"NX2NPBIN", version, the driver's global key size and bytes} (another global key: another driver or GPU,
so the file starts empty); then records {u32 bytes after this field, u32 type, u32 key size, 32 key bytes, payload}:
type `PB_BLOB`, a binary under its binary key, payload its data; type `PB_PIPE`, a pipeline under its pipeline key,
payload {u32 count, count x {u32 key size, 32 key bytes}} naming its binaries. Only an index of the records stays in
memory; a pipeline's records are read from the file when it is created. `NX2SARIA_PIPE_BINARIES_MAX_MB` (default 1024, 0
no limit): a larger file starts empty.

`pb_seek`, `pb_tell`: 64-bit file offsets: `long` is 32 bits on Windows, and a file over 2 GB read as cut short.

- `#define PB_HEAD (4 + 4 + PB_KEY)`: a record's bytes after its size field and before its payload.
- `PbSlot.off`: the file offset of the record's size field.

`PB`:

- `PlatMutex lock`: the index, the pending records and `rf`.
- `FILE *rf`: reads records.
- `uint8_t *pend`: records not written yet; they follow the file's end in this order.
- `uint64_t file_size`: bytes on disk.
- `int no_capture[3]`: the kinds (`pipe_create()`) whose create refused the capture flag.

Two values a thread keeps of its last create:

- `pb_last`: 1 when the last `pipe_create()` on this thread made its pipeline from binaries.
- `cc_hit`: `compute_create()`'s last pipeline on this thread: 1 the pipeline cache held it (`P.cache`: read from
  `pipeline-cache.bin`, or built into it earlier in this run) or it was made from pipeline binaries, 0 not, -1 the
  driver did not say.

### `pb_find()`

Caller holds `PB.lock`.

### `pb_insert()`

Enters a record (a hash match is taken as the key: 64 bits). 0 when one with the key is there already or out of memory.
Caller holds `PB.lock`.

### `pb_rewrite()`

An empty file with this driver's header.

### `pb_index()`

Indexes the records of `f` (`len` bytes) after its header: per record its type, key and offset. Returns the offset
after the last whole record, which is `len` unless the file ends inside a record (an append the process did not finish)
or holds a malformed one.

### `pb_keep()`

The length of the file at `path` (`len` bytes) to go on with after `pb_index()` found its whole records ending at
`good`: `len` when they fill it. When it ends inside a record (the process ended during an append, or a disk filled) or
holds a malformed one, the records from there on are dropped and the file is cut back to `good`, so that appends go on
after the whole records before it. 0 when it cannot be cut, and the file starts empty.

### `pb_init()`

Opens `pipeline-binaries.bin` and indexes its records (`cache_load()`, before `P.cache`). Without one of the
extension's functions or without the driver's global key the run goes on with `pipeline-cache.bin`.

The file starts empty (the index dropped, the file rewritten with this driver's header) when there is none, when its
header names another driver, GPU or version, when it is over `NX2SARIA_PIPE_BINARIES_MAX_MB`, and when `pb_keep()`
cannot cut it back to its whole records. A file that only ends inside a record or holds a malformed one keeps the
whole records before it.

### `pb_read()`

The payload of the record of (type, key), malloc'd (`*size` bytes), or NULL. Caller holds `PB.lock`.

A record whose offset is at or past `PB.file_size` is still pending and is read from its place in `PB.pend`; the others
are read through `PB.rf`, which is NULL only after `pb_flush()` could not open the file again. The record counts only
when its type and the key itself match, not only the key's hash.

### `pb_load()`

The binaries stored for pipeline key `k`, created in `bins` (`*n` of them). 0 when none are stored or they fail.

### `pb_put()`

Appends one record to `PB.pend` and the index. Caller holds `PB.lock`.

### `pb_capture()`

Stores the binaries of pipeline `p` (created with its data captured) under pipeline key `k`, then frees the data. For
each binary it asks the key first, and the data only when no binary with that key is stored yet (`PB.blob_shared`
counts the others). The lock is not held between that question and `pb_put()`, so a 0 from `pb_put()` means another
thread stored the same binary meanwhile. A cold boot captures thousands of pipelines before the autosave thread runs:
once 64 MiB are pending it calls `pb_flush()` itself, to keep the buffer small.

### `pb_flush()`

Appends the pending records (the autosave thread every 2 s, and the save at exit).

When the append fails, part of it may be in the file (a full disk): the file is cut back to `PB.file_size`, where the
index places the pending records, or the next append would land after those bytes. The read handle is closed meanwhile,
since `plat_file_truncate()` (`runtime/platform/platform.h`) opens the file alone. The pending records stay in memory
and are tried again at the next flush.

### `cache_build_enter()`

Takes `cache_rw` shared for a build into `P.cache`; while the save at exit waits for its copy, first waits for that.

`cache_rw` keeps builds into `P.cache` apart from reads of its data. Every create with `P.cache` holds it shared
(`pipe_create_in()`); a save holds it exclusively for the copy of the data and only takes it when no build is running.
The Vulkan rules allow reading a cache created without `VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT` while
pipelines are built into it, but Intel's Windows driver (101.8991 on an Arc 140V) crashed inside
`vkGetPipelineCacheData` during the copy, reading through a null entry, with compute and library builds running beside
the autosave. So no copy is made while a build runs, on any driver.

`cache_saving_at_exit` is set while the save at exit waits for `cache_rw`: the builds already in the driver finish, and
new ones wait in `cache_build_enter()` until the copy is made. Without it the boot's prewarm workers started builds back
to back, no moment without one came within the save's 5 s, and the pipelines built in that run were not saved (a window
closed during the prewarm, 2026-11-11). The autosave never sets it, so it never holds a build back.

### The play cache

`NX2SARIA_PIPELINE_CACHE_PLAY`, default 1; needs `pipelineCreationCacheControl`.

`pipeline-cache.bin` holds what the preparation built and what earlier runs made: 300 MB and more on an install. One
file can only be written whole, so a single new pipeline in play had the whole of it copied out of the driver and
written again (2026-11-11: saves of 358.1 MB for 0.3 MB of new data, and 302 to 306 MB written in each of seven sessions
that added about 0.5 MB each). So what a run compiles goes into a second cache, `P.play`, with a file of its own,
`pipeline-cache-play.bin` beside the first: a create asks `P.cache` with
`VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`, which gives the pipeline when that cache or the driver's own
has it and `VK_PIPELINE_COMPILE_REQUIRED` when not, and only then builds into `P.play` (`pipe_create_in()`). In play
only the play file is saved; `pipeline-cache.bin` is not written, except once at the exit of a run that started without
it (`P.base_at_exit`: the translator stamp had deleted it (`cache_stamp_check()`), a run before ended without that save,
or no preparation ever wrote it).

The play file is read back at the next start, into `P.play` and merged into `P.cache` in memory (`cache_load()`), so the
first create finds its pipelines. Once it has grown to `NX2SARIA_PIPELINE_CACHE_PLAY_MB` (default 64) it is merged into
`P.cache` at the start, before play: the autosave thread writes `pipeline-cache.bin` once and removes the play file
(`cache_save()`). The preparation modes (`--prepare-shaders`, `--warm-shaders`, `--prepare-pipelines`) merge it at any
size and build into `P.cache` alone, as before. A pipeline the driver's own cache gives to the first create enters
`P.cache` in memory only: it is in a file again when the play file is next folded in, when the driver no longer has it
and it is compiled, or at the exit of a run that started without `pipeline-cache.bin`.

Without the device feature, with pipeline binaries on, or with the setting 0, `P.play` stays `VK_NULL_HANDLE` and
everything goes into `P.cache` as before.

`prep_mode`: the run is a preparation mode (set before `ensure_init()`), which has no play cache.

### `pipe_create_in()`

One create, into the cache the thread and the run call for. With a play cache, a create that would go into `P.cache`
first asks it with `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`; a pipeline that is neither in `P.cache`
nor in the driver's own cache then comes from `P.play`, or is compiled into it. A caller that asked for cached
pipelines only (its own `FAIL_ON_PIPELINE_COMPILE_REQUIRED`) gets the play cache's answer. The `NX2SARIA_TRACE=perf`
mark covers the create on any thread, its wait for `cache_rw` included.

Three values choose the cache:

- `own_cache`: a pipeline cache of this thread's own that its creates build into in place of `P.cache`;
  `VK_NULL_HANDLE` (every thread but the background builder of large compute shaders) for `P.cache` itself. A create
  into it does not hold `cache_rw`, so a build of minutes does not keep `pipeline-cache.bin` from being saved;
  `bg_merge()` moves its content into `P.cache`.
- `no_cache`: 1 on a thread whose creates take no pipeline cache of ours at all (the library warm in play,
  `lw_worker()` and `lw_probe()`): what it builds is wanted in the driver's own disk cache only. Built into `P.cache`,
  the 37,572 stage libraries of a warm grew it by 1.6 GB (2026-11-11: a save of 2026.7 MB in play, +1612 MB since the
  boot); the file was then over `NX2SARIA_PIPELINE_CACHE_MAX_MB`, so the next start dropped it whole and compiled the
  pipelines it had held.
- `cache_merges`: new content in `P.cache` that `P.stats` does not count, for `autosave_once()` to save: `bg_merge()`'s
  merges, the compiles of pipelines built for the cache only (`lb_compute()` with `keep` 0), and the creates of
  `vk_pipeline_create_foreign()`.

### `vk_pipeline_create_foreign()`, `vk_pipeline_foreign_counts()`

The pipelines of a library that draws on our device (FSR 3.1: `runtime/vk/vk_ffx.cpp` `vk_ffx_proc_addr()`).

Such a library creates its pipelines itself and passes no pipeline cache, so nothing of ours kept them: every context it
made (one per render and output size, and one for frame generation) compiled its passes again unless the driver's own
disk cache still had them, which a renamed or updated exe on NVIDIA and a driver without a disk cache do not.
`runtime/vk/vk_ffx.cpp` hands the library these creates in the place of `vkCreateComputePipelines` and
`vkCreateGraphicsPipelines`; a create that names no cache goes through ours as `pipe_create_in()` does: asked of
`P.cache` first without compiling, and compiled into the play cache when neither it nor the driver has the pipeline, so
the file a run writes (`pipeline-cache-play.bin`, or `pipeline-cache.bin` without a play cache) holds it for the next
start. Before the first pipeline of ours (`P.ready` 0) and with pipeline binaries on there is no cache to give: the
create goes to the driver as the library asked. `infos`: `n` `VkComputePipelineCreateInfo` (`compute` 1) or
`VkGraphicsPipelineCreateInfo`. Any thread.

- Without a play cache there is one cache, and whether it held the pipelines is not known here: its save looks at the
  data's size. `vk_pipeline_foreign_counts()` then gives them as made, neither from the cache nor compiled.
- With a play cache each create info is copied: the flags are ours to change, the library's struct is not.
- Flags given in a `VkPipelineCreateFlags2CreateInfo` replace the struct's own, so the question "without compiling"
  cannot be put through `hd->flags`: such a create goes to the play cache at once.
- A pipeline compiled into the play cache adds to `cache_merges`, so `autosave_once()` sees that the play file is due.
- The result is that of the first create that failed, as a create of several reports one result.

### `pipe_create()`

Creates one pipeline: `kind` 0 a graphics pipeline, 1 a graphics pipeline library, 2 a compute pipeline; `ci` its
`Vk*PipelineCreateInfo`. With binaries on: from the stored binaries of its pipeline key when there are any, else
compiled with `P.cache` and its binaries captured. The create info is restored before returning.

A pipeline made from binaries takes no pipeline cache. When the driver refuses a create with the capture flag, that
kind is created without it from then on (`PB.no_capture`, with a log line) and its pipelines are not stored.

### `cache_cap()`, `cache_data_read()`

The data of the pipeline cache file at path for `vkCreatePipelineCache` (malloc'd, its size in `*size`), NULL when the
run starts without it, which the log says with the file's size. Cache data whose header names another device or driver
is dropped before Vulkan sees it.

`NX2SARIA_PIPELINE_CACHE_MAX_MB` (`cache_cap()`; 0 no limit): a larger file starts the run empty, and the run's saves
replace it. The limit bounds each of the two files, not their sum. It has to stay above what one run builds: at 512,
with the pipeline list of 0.9.1 (8,849 records), an RTX 2080 Ti's file held 551 MB and the play file the run then wrote
541 MB, so both were dropped at every start, 7,247 of 8,531 pipelines were built again in each (17 s of the boot) and
541 MB were written again in each session (`cache_load()` says so when both files are over). The file only grows:
entries of earlier translators and builds stay, and on NVIDIA (621.01) an entry another executable wrote is neither used
nor written again, so a renamed or updated exe compiles while the file keeps its size (2026-11-09: 650 MB, 15-18 % of
the pipelines read from it). The whole file sits in process memory twice at the boot (this read and the driver's copy)
and is copied out at every save of it, which a 16 GB machine near its commit limit cannot spare. The size is checked
before the read, so a file that is dropped never takes its size in memory. Sizes are 64-bit all the way
(`runtime/vk/vk_cache_file.h` `cache_file_read()`): a file past 2 GB is read with the limit raised or off, and refused
by its real size otherwise.

The limit is kept on the write side too (`cache_load()`'s fold, `cache_save_one()`): a run never writes a file the next
start would refuse. Before, only the read checked it, all or nothing, and nothing kept the files under it: on an RTX
5080 (0.9.2, 2026-11-12) `pipeline-cache-play.bin` reached 524.9 MB in one session of about 15 minutes, the next start
dropped it whole ("starting empty") and the session after compiled its pipelines again (boot hits 13-18 %, about 5,000
pipelines left to background builds at 250-273 ms each). `pipeline-cache.bin` itself grows 14-28 MB at each fold (323 ->
448 -> 461 -> 489 MB in four starts) and so crosses 512 MB in a few sessions (0.8.0: a 766 MB file dropped at a start).
With the limit at 2048 MB the same files were read and folded: boot hits 84 %, nothing left to background builds, the
boot 17 s instead of 24.

`cache_cap()`: `NX2SARIA_PIPELINE_CACHE_MAX_MB` when set (0: no limit); unset, the machine's RAM / 32 between 1024 and
4096 MB (16 and 32 GB: 1024; 64 GB: 2048; 128 GB and more: 4096). Never below 1024: a full cache of the pipeline list
is about 550 MB on an NVIDIA card, and a limit under what the files reach compiles everything past it again at every
start, for good (RAM / 32 alone gave 509 MB on a 16 GB machine). The RAM and not the commit headroom at the boot, so
the same file is not read on one start and refused on the next. Read once, at the start; every caller computes the same
value, so the unlocked cache of it is harmless.

- `cache_over_cap`: the last file `cache_data_read()` was asked for is over the limit. Such a file comes from before
  the limit was kept on the write side, or from a lower limit.
- `cache_foreign`: `pipeline-cache.bin` was written by another device or driver (a driver update).

### `cache_make()`

A pipeline cache from data (may be NULL: empty); `VK_NULL_HANDLE` when the driver makes none. `*kept`: the size of the
data it holds afterwards. A driver may drop initial data it does not accept without failing the create, which leaves
every build of the run compiling while the cache file looks valid.

### `cache_stamp_check()`

The translator stamp of the two pipeline cache files (`runtime/vk/vk_cache_stamp.h`, where the rules are):
`pipeline-cache.id` beside `pipeline-cache.bin` names the translator whose pipelines the files hold. Called by
`cache_load()` before either file is read. A stamp of another translator whose shaders the carry no longer serves (or
any other translator in a preparation: `prep_mode`) deletes both files, so their dead entries are neither read nor held
twice in memory nor carried on by the next save; the play file goes too, since what one run compiled under the earlier
translator is as dead. After a deletion outside a preparation the run writes `pipeline-cache.bin` again at its exit
(`cache_load()` sets `P.base_at_exit` by `runtime/vk/vk_cache_stamp.h` `cache_base_at_exit()`), and the line says so.
One log line says what a start that reads the files did and how many MB they held. `NX2SARIA_PIPELINE_CACHE_STAMP`
(default 1); 0 reads the files whichever translator wrote them and leaves the stamp as it is.

- A file that is still there after the deletion is held open elsewhere: the files are read as before and the stamp is
  kept.
- The line also applies the rule `cache_load()` then sets `P.base_at_exit` by: a start without `pipeline-cache.bin`
  writes it at its exit.
- The line: `vk: pipeline cache stamp <id> matches|differs|none: <action>, <MB>`, then what the numbers need to be
  checked (the files' translator when it differs, whether `pipeline-cache.bin` is written at the exit, the stamp's
  path).

### `cache_load()`

Reads the two cache files and decides what the run does with the play file, in this order:

1. With pipeline binaries on (`PB.on`) the binaries carry compiled pipelines across runs: `P.cache` stays
   `VK_NULL_HANDLE` and neither file is read.
2. `cache_stamp_check()`. No `pipeline-cache.bin` after it (deleted by it, or the run that started without it ended
   before writing the file again): the save at exit writes it (`P.base_at_exit`; `cache_save()`,
   `runtime/vk/vk_cache_stamp.h` `cache_base_at_exit()`).
3. `pipeline-cache.bin` into `P.cache`. An oversized file is never read again: the play file takes its place at once
   (`base_over`). `P.saved[0]`: the file holds this data when the driver took all of it, so a save is due only once the
   cache has grown (`cache_save()`). Without data read from the file the empty cache's own size counts as saved, as the
   play cache's does in step 7: a run that took nothing into it does not write a file of a header alone, which the next
   start would refuse as too short (`runtime/vk/vk_cache_file.h` `cache_file_read()`) while it no longer counts as
   missing (`P.base_at_exit`).
4. The play file ("The play cache"). When both files are over the limit, what one run builds is over it and nothing
   will ever replace either file: the log says so and how to raise the limit.
5. A fold with the play cache on (the play file reached `NX2SARIA_PIPELINE_CACHE_PLAY_MB`) is taken only when its
   result fits: `pipeline-cache.bin` past the limit would be dropped whole at the next start (`cache_data_read()`). The
   merge into `P.cache` is the one the kept play cache gets in step 7 anyway, so it is done here once and its size
   asked; a play file mostly holds what the other already has (461.2 + 524.9 MB folded to 489.0 MB, 2026-11-12), so the
   size of the two files says little. The fold is written at the first autosave, after the boot's prewarm and
   background builds have added to `P.cache`: it is taken only with room for a play file's worth of growth (`fold_at`)
   under the limit, so that save is not refused (`cache_save()` keeps the play file meanwhile). A fold not taken leaves
   both files as they are and keeps the play cache, already merged, so step 7 is skipped for it.
6. The other folds (a preparation, the setting 0, no `pipelineCreationCacheControl`, an oversized
   `pipeline-cache.bin`): the play file is merged into `P.cache` now; the file is written by the first save, which then
   removes the play file. A run that ends before that save finds both files as they were and merges again.
7. A play file that is not folded is kept as the play cache, so its file goes on holding what earlier runs made, and
   merged into `P.cache` in memory, where the first create of `pipe_create_in()` finds its pipelines. Without the merge
   that create took them from the driver's own cache when it had them, at 8 ms a pipeline against 0.4 ms from ours.
8. `P.saved[1]`: an empty play cache is not written (its data is a header): its size counts as saved, as a whole
   file's does.

### `mesh_stages_init()`

The two settings for the task and mesh stages, and one line with what they do on this device (`sg_min`, `sg_max`: its
subgroup size range, 0 without subgroup size control).

- `NX2SARIA_VK_MESH_SUBGROUPS` (`stage_sg()`): the translator writes task and mesh programs for 32-lane subgroups, as it
  does compute programs. The mesh stage was already pinned to 32, the task stage was not, and neither asked for full
  subgroups, so a driver whose subgroups run 16 to 32 lanes (Intel Arc) could run a task workgroup as partial or 16-lane
  subgroups. The task program hands out mesh work by ballots over its lanes, and the fog's depth pre-pass and colour
  pass, two pipelines with the same task program, could then emit different sets of fog quads: the suspected cause of
  the holes an Arc B390 showed in Kokiri Forest's fog (issue #13), which the mesh fallback, whose task programs run as
  compute with these requirements, did not show. Where subgroups are always 32 lanes (NVIDIA), pinning changes nothing.
- `NX2SARIA_VK_MESH_INVARIANT` (`spirv_mesh_invariant()`).

### `nan_preserve_init()`

The float widths `spirv_nan_preserve()` declares on this device (`P.nan_preserve`), from `NX2SARIA_VK_NAN_PRESERVE`: 0
off, 1 on every driver; unset, on every driver except NVIDIA's own, on which the game draws right without it. Only
widths the driver can preserve (`VkPhysicalDeviceFloatControlsProperties`) are declared, and nothing without 32-bit
floats.

`NX2SARIA_VK_NAN_BITS` (`P.nan_bits`, `spirv_nan_bits()`): 1 every driver, 0 none; unset: AMD's driver for RDNA 1 and 2
(`vk_caps.amd_sc`).

### `mesh_lib_init()`

Stage libraries for mesh pipelines.

`NX2SARIA_GPL_MESH`, default 1; 0: each mesh pipeline is built whole, as before.

A mesh pipeline (task and mesh stages: the game's foliage, grass and terrain) used to be left out of the stage
libraries: each new one was a whole build, 0.2 to 1.1 s the first time foliage of a kind appeared, and its draws waited
or were left out of the picture until it was done. With the setting on such a pipeline is made like a vertex pipeline
("graphics pipeline library" below): linked at its first use from libraries, with the whole build behind.

- Its pre-rasterization library holds the task and mesh stages with the viewport and rasterization state, keyed by both
  shaders (`PrKey.vs`, `PrKey.ts`). A mesh pipeline has no vertex input state, so it is linked from three libraries and
  none of them is a vertex input library (`gpl_link()`).
- Its layout is a stage-independent pair like a vertex pipeline's (`mesh_pair_layout()`): set 0 holds the mesh stage's
  bindings and the task stage's after them, set 1 the fragment stage's. Without libraries all three share set 0.
- Its fragment shader library is the one a vertex pipeline with the same fragment shader uses, unless the key says
  otherwise (FsKey): the inputs no mesh output feeds are filled in the module (fill, points), and on a device with a
  per-draw shading rate the rate is dynamic state of every mesh pipeline, which the pre-rasterization and the fragment
  shader state both hold, so both libraries list it (vrs). The fragment output library is shared.
- The pre-rasterization library lists the dynamic states a whole mesh pipeline lists (`mesh_dyn()`): not the vertex
  input, its strides, the topology or primitive restart, which a pipeline with a mesh stage may not list. So does a
  fragment shader library that is a mesh pipeline's alone (vrs). A library shared with vertex pipelines still lists
  those four: they belong to the vertex input state, which no library of a mesh pipeline holds, and what a library lists
  as dynamic counts only for the state the library itself holds.

Off, with the reason in the start's log line: without the graphics pipeline library, on a device that draws no mesh
pipeline (no mesh shaders, or the mesh fallback), and where mesh pipelines alone are built without the driver's
optimiser (`noopt_flag_for()`): a fragment library shared with vertex pipelines would be an optimised one.

### `ensure_init()`

- `runtime/vk/vk_device.c` enables every feature the GPU reports in this chain, so the query tells what is enabled.
- Maxwell warps have 32 lanes; the translator assumes it ([maxwell.md](../maxwell.md) 3.4). Pin 32 where the driver
  allows it.
- `P.sg32_stages = vk_caps.sg_pin_stages`: `runtime/vk/vk_caps.c`: 32 unless `NX2SARIA_VK_SUBGROUP`; 0 without the pin.
- A warp reconverges where the guest compiler put it back together; Vulkan guarantees that only with maximal
  reconvergence (`spirv_reconverge()`). `NX2SARIA_VK_RECONVERGE=0` leaves the modules as translated.
- Pipeline creation feedback is core in Vulkan 1.3; the driver reports per pipeline whether `P.cache` held it.
- Descriptor update templates (core 1.1; the push variant comes with `VK_KHR_push_descriptor` on 1.1): the driver walks
  a layout prepared when the layout is made instead of parsing write structs every draw. `NX2SARIA_DESC_TEMPLATE=0`
  keeps the write structs.

`NX2SARIA_VK_DYN_VERTEX=0`/1 forces dynamic vertex input off or on. Default off on two drivers:

- Intel's frozen Windows driver for its older integrated GPUs (UHD 630, Ice Lake: Vulkan 1.3.215 and older), which draws
  nothing with it.
- AMD's driver for RDNA 3 and newer (`vk_caps.amd_llpc`), which compiles a pipeline with dynamic vertex input again at
  every boot: neither `pipeline-cache.bin` nor the driver's own disk cache gives it back. Measured on an RX 7900 XT
  (26.10.1) with `NX2SARIA_PIPE_FEEDBACK=2`, booting one save twice from an empty cache: the second boot compiled all
  622 recorded non-mesh pipelines again at 83 ms each, with the module and state hashes of the first boot (the same with
  `NX2SARIA_GPL=0`, so it is not the pipeline libraries or their layouts), while the mesh pipelines, which have no
  vertex input, came from the file. With the vertex layout in the pipeline the second boot built 787 pipelines in 170 ms
  and the third 898 of 898 from the file in 233 ms, with no build left for the scene. A player's file of 3,979 records
  used the whole boot budget (`PREWARM_PIPE_MS`, 20 s) at every boot; with the vertex layout in the pipeline its third
  boot built 4,273 pipelines in 0.9 s. The price is more pipelines, one per vertex layout (787 records against 692 for
  that boot); records written with dynamic vertex input still prebuild, each for the layout of the draw that recorded
  it. The RDNA 1 and 2 branch (`vk_caps.amd_sc`) is not measured and keeps dynamic vertex input.

`NX2SARIA_VK_DYN_A2C=0`/1 forces dynamic alpha to coverage off or on. Default off on AMD's Windows driver branch for
RDNA 2 and older (`vk_caps.amd_sc`), which applies alpha to coverage while the dynamic state says off: on an RX 590
(24.9.1) the game, which never turns it on, drew Link and the NPCs with depth but no colour (black silhouettes) and
dithered other surfaces into checkerboards (issue #67, RX 470/570/580). Taken out of the dynamic state, it goes into
the pipeline key and the characters draw. With the texture pool marks it stays out of the dynamic state whatever the
setting: a marked fragment shader gets early fragment tests only in a pipeline that has alpha to coverage off
(`spirv_pool_mark()`), so the pipeline has to know.

After `cache_load()` it calls `pg_draws_min()` for the play gate's line at the start.

### `compare_op()`, `stencil_op()`, `polygon_mode()`

The NVN values map to Vulkan's by arithmetic:

- `NVN_DEPTH_FUNC_*` / `NVN_STENCIL_FUNC_*` are NEVER = 1 .. ALWAYS = 8 in `VkCompareOp` order.
- `NVN_STENCIL_OP_*` are KEEP = 1 .. `DECR_WRAP` = 8 in `VkStencilOp` order.
- `NVN_POLYGON_MODE_POINT`/LINE/FILL = 0/1/2, `VkPolygonMode` FILL/LINE/POINT = 0/1/2. Without `fillModeNonSolid`
  everything is FILL.

### `vk_pipeline_topology()`

A quad strip is drawn as a triangle strip: the same triangles, the same coverage. The default, a triangle list, is
TRIANGLES, and QUADS after the replay's index rewrite.

### `topology_class()`

Topology class (a pipeline's static topology only fixes the class): 0 point, 1 line, 2 triangle, 3 patch.

### `vk_pipeline_tex_type()`

The default, `GFX_TEX_2D`, is for the 2D and RECTANGLE targets.

### `format_type()`

Numeric type of a colour format as a shader sees it: `GFX_TYPE_*`.

### `format_features()`

Optimal-tiling (`optimal` 1) or buffer (`optimal` 0) features of a core format, cached.

### `vk_vertex_format()`, `widen()`

`vtx_formats` (`VtxFmt`) gives the vertex attribute format of each `NVNformat`, with the type a shader reads it as.

- `*_UI2F` / `*_I2F` (0x51 to 0x6C): integer data read as float; the 8- and 16-bit ones are Vulkan's SCALED formats.
- The 32-bit ones have no SCALED format: read as integers (values are not converted; logged when used).
- `widen()`: 3-component 8/16-bit formats are not vertex formats on every GPU: such a format is read with 4
  components, and the log says so once.

### `target_format()`

Attachment format of an NVN format (`tex_vk_format()`: the format the images are created with).

### `zero_format()`

An attribute the game left without a format is a fixed one on Maxwell, a single 32-bit component of 0 (an unused
attribute is bound as IsFixed 1x32 float): it reads (0, 0, 0, 1), as Vulkan expands a one-component format. The game's
foliage shaders scale wind sway by 1 - colour.w; shapes without a vertex colour (Brightbloom stem and leaves) must not
sway, or they bend away from the parts that have one (the bulb).

### `vertex_input()`

The vertex input, which is pipeline state or goes through `vkCmdSetVertexInputEXT`: attributes for the Locations the
vertex shader reads; one without a usable NVN attribute reads `VK_PIPE_ZERO_BINDING`. An instance divisor above 1 is
treated as 1 (logged once).

### `spirv_rebind()`

Binding decorations b >= 1 become b + `off` (the fragment stage's share of set 0).

### `spirv_set()`

DescriptorSet decorations become `set` (the fragment stage's resources move to set 1 with independent layouts).

### `spirv_dual_source()`

Dual-source blending ([maxwell.md](../maxwell.md) 5.6): SRC1 blend factors read the fragment shader's colour output 1,
which Vulkan takes from Location 0, Index 1. The Output variable at Location 1 moves there, as the translator declares
it with DualSourceBlendEnable (`tools/vendor/ryujinx/src/Ryujinx.Graphics.Shader/CodeGen/Spirv/Declarations.cs`); the
pipeline is keyed by `GKey.dsb`, so only pipelines that blend with SRC1 get this module. NULL when the module has no
Output at Location 1.

### `spirv_add_outputs()`

Adds an Output variable for each Location of `missing` (type and width from `io`) to a module, declared in the entry
point interface and initialised as Maxwell reads an unwritten output: (0, 0, 0, 1), the w as the bits of 1.0f whatever
the type (attribute memory is untyped); fewer than 4 components are all zero. Existing scalar, vector and pointer types
are reused (`scalar[]` is indexed by `GFX_TYPE_FLOAT`, SINT, UINT). NULL when the module does not have the expected
layout.

### `spirv_mesh_outputs()`

The locations a mesh module writes (its Output variables with a Location), and whether it draws points (execution mode
OutputPoints). 0 when the module cannot be read.

### `spirv_fs_fill_inputs()`

A fragment module whose inputs at the locations of `missing` read what Maxwell gives a fragment input no earlier stage
writes: (0, 0, 0, 1), or with `points` the point sprite's coordinate (s, t, 0, 1). Each such Input variable (a float or
a float vector) becomes a Private one, the access chains through it Private pointers, its decorations go, and the entry
function stores the value into it before anything else. NULL when nothing is to be done or the module does not have the
expected layout.

How it goes through the module:

1. Pass 1 finds the variables to fill and the Private pointer types they and the chains through them need (`priv`:
   Input pointer type -> Private). `fill[id]` is 1 for a filled variable and 2 for an access chain through one.
2. The globals added before the first function: two constants, `gl_PointCoord`, and the Private pointer types not yet
   declared (a filled variable's goes right before the variable; an access chain's is only used in a function).
3. The entry point's interface: filled variables out before SPIR-V 1.4 (Private is not listed), `gl_PointCoord` in.
4. The decorations of a filled variable go: Location, Flat and the like belong to Input variables. `gl_PointCoord`'s
   BuiltIn comes after the last decoration.
5. In the entry function's first block, after its OpVariables, the stores: x y z w in order, (s, t, 0, 1) or
   (0, 0, 0, 1), cut to the variable's width.

### `spirv_fixup()`

Two fixes for what the translator declares but a shader does not use (NULL: nothing to change):

- The fragment shader interlock capability and extension, which the translator adds to every fragment shader when the
  host reports interlock support (`tools/vendor/ryujinx/src/Ryujinx.Graphics.Shader/CodeGen/Spirv/SpirvGenerator.cs`)
  and `runtime/vk/vk_device.c` does not enable; kept when the shader interlocks;
- The `CullDistance[1]` member of `gl_PerVertex` next to `ClipDistance[8]` (`Declarations.cs`
  CreatePerVertexStructType): 9 distances exceed `maxCombinedClipAndCullDistances` (8 on the RX 9070). The translator
  never accesses CullDistance; the member goes when it is the struct's last and no access chain selects it.

An access chain into an arrayed per-vertex block has the member one index later. The struct loses its last member by
its word count going down by one.

### `uses_subgroup_ops()`

True when the module declares a `GroupNonUniform*` capability (61 to 68), SubgroupBallotKHR (4423) or SubgroupVoteKHR
(4431): lane masks assume 32 lanes. Capabilities come first, so it stops at the first instruction that is not an
OpCapability.

### `spirv_reconverge()`

Guest code runs warp-wide operations (ballot, shuffle, vote) where the guest compiler had the warp reconverged, for
example right after a loop that each lane leaves at a different iteration. Vulkan only promises the same lanes at the
same point with the MaximallyReconvergesKHR execution mode. Without it a driver may run the lanes that left the loop
early through the operations on their own, with a ballot or scan that sees only some of the lanes: a mesh program that
hands out output slots from one could give two primitives the same slot. This adds the extension and the mode for each
entry point of a module that declares subgroup capabilities. NULL: nothing to change (or no memory).

### `spirv_mesh_invariant()`

The game draws a fog sheet twice: a depth pre-pass, then a colour pass that discards where `gl_FragCoord.z` is behind the
pre-pass's depth. Both are mesh pipelines with different mesh programs, so on Maxwell the sheet's positions match bit
for bit, and the comparison relies on it. The translator marks a vertex shader's position Invariant
(`tools/xlate/patches/0007-position-always-invariant.patch`), which
holds the driver to computing the same expression the same way in every pipeline; the mesh modules it makes
(`tools/xlate/MeshSpirv.cs`) declare the position without it. This adds Invariant to the Position built-in of a MeshEXT
module, as OpDecorate on a variable or OpMemberDecorate on a block member, right after its BuiltIn decoration. NULL:
nothing to change (not a mesh module, no position, Invariant already there, or no memory). Once
`tools/xlate/MeshSpirv.cs` declares it, this finds it there and changes nothing.

### `spirv_nan_preserve()`

Maxwell computes in IEEE 754, and the game's shaders rely on it: its compiler writes an unordered comparison as `x > c
|| isnan(x)`, a normalisation of a zero-length vector gives an infinity that a later comparison sorts out, and integer
masks such as 0xffffffff travel through float moves and selects. The translator's own guards are isnan tests as well
(fragment outputs, min, max and clamp). Without the SignedZeroInfNanPreserve execution mode, SPIR-V lets the driver
assume that no float is NaN or infinite, and AMD's compilers act on it: on an RX 9070 XT a select between 1 and the bits
0xffffffff, converted to float as a signed integer, gave 2^32 instead of -1 and blew the shadow casters up into huge
triangles (issue #156); RX 6000 cards drew Navi, and Link's skin next to her, with black and white blocks (#41, #110);
and AMD GPUs drew Navi's sparkles but not her orb, whose light sits at her own centre. NVIDIA's driver keeps the IEEE
results without the mode. This adds the capability, the extension (the translator writes SPIR-V 1.3) and the mode for
each entry point: 32-bit floats always, 16-bit and 64-bit ones where the module declares Float16 or Float64 and `widths`
(`NP_*`) allows them. NULL: nothing to change (the module has the mode, or no 32-bit width is allowed, or no memory).

### `spirv_nan_bits()`

NaN tests that no compiler can drop (`P.nan_bits`). The game's shaders and the translator's own guards test for NaN with
OpIsNan: 48,717 of an install's 54,959 translated modules, 26 tests each on average (the game's unordered comparisons,
and the guard that keeps NaN out of every float colour output, patch 0027, so that one NaN texel does not spread through
bloom and blur into blocks of black and white). SignedZeroInfNanPreserve asks the driver to keep NaN, but whether a
compiler that reasons about floats keeps an isnan through every optimisation is up to it. The reports of black and white
blocks, on Navi, on Link next to her and on bright surfaces seen from inside a house, come from RX 5000 and 6000 cards
(#41, #110, #117), whose Windows driver compiles with "AMD proprietary shader compiler" (`vk_caps.amd_sc`); RDNA 3 and 4
(LLPC) draw the same scenes clean. It is not known whether that compiler keeps the mode: on it each float32 test becomes
integer arithmetic on the bits, which means the same under any float assumption: isnan(x)  ->  (bitcast<uint>(x) &
0x7fffffff) > 0x7f800000 for scalars and vectors of 32-bit floats; tests of 16- and 64-bit floats stay. A float32 select
with a constant operand whose bits are a NaN (an integer mask such as 0xffffffff that the translator types as float)
becomes a select of the bits (issue #156: on an RX 9070 XT without the mode such a select between 1 and -1 gave 2^32).
NULL: nothing to change (or no memory); `*tests` and `*selects` count what changed.

- Inside functions the result type of an id is taken from the second word of every instruction but those without a
  result: the first assignment wins anyway, as a definition comes before its uses (OpPhi operands aside, which this
  never reads).
- The declarations go right before the first function: uint32 and its vectors (by component count) where missing, then
  the constants.

### `vk_pipeline_spirv_selftest()`

`--selftest` (`runtime/main.c`): `spirv_nan_preserve()` on a small fragment module, without a device. Returns the number
of failures.

The module declares Shader and Float64. Three cases: every width allowed and Float64 declared (a 64-bit mode is
expected), only 32 bits allowed with Float64 declared, and Matrix in place of Float64; then a call without the 32-bit
width, which must change nothing. `spirv_nan_bits()` gets a module with a scalar and a vec2 isnan and a select on a NaN
constant, and no uint32 type. With `NX2SARIA_SPV_MARK_TEST` it also runs `spirv_mark_list_test()`.

### `pin_subgroup32()`

Chains the pinned subgroup size (`vk_caps.sg_size`, 32 unless `NX2SARIA_VK_SUBGROUP`) into a stage create info when
the stage uses subgroup ops and the driver allows the pin.

### `local_size_x()`

The x of a module's LocalSize (OpExecutionMode 17), 0 when it has none. The execution modes are all before the first
OpFunction, where it stops.

### `stage_sg()`

`StageSg`: a task or mesh stage's subgroup requirements under `NX2SARIA_VK_MESH_SUBGROUPS` (`mesh_stages_init()`), from
its SPIR-V while the caller still has it: pin, 32 lanes (the task stage; the mesh stage takes `pin_subgroup32()` as
before), and full, every subgroup of the workgroup full, where its width is a whole number of 32-lane subgroups.

### `stage_sg_apply()`

Puts `g` on a stage's create info; `rq` must live as long as it. The mesh stage's pin is already chained
(`pin_subgroup32()`). Full subgroups are asked only with the size pinned: the requirement is then a multiple of it.
The log counts the stages so far at the 1st, 10th, 100th and 1000th.

### `spirv_pool_rebind()`

Pool arrays (the translator's bindless texture and sampler arrays of `VK_POOL_TEX` / `VK_POOL_SAMP` elements, found by
their types) move to set 2, at the binding of their view kind (`runtime/vk/vk.h` `VK_POOL_KINDS()`) or `VK_POOL_KINDS`
for samplers; the kinds used are noted in `vk_pool_kinds_used` so the replay fills them. NULL: nothing to move (or no
memory).

- `kind[id]`: for a type, its image kind (sampled images only), 100 for a sampler, -1 other; + 1000 for a pool array
  and a pointer to one, + 2000 for a pool variable.
- Pool variables and their binding decorations are module declarations, all before the first function.
- A translation carried over from a translator whose pool was `VK_POOL_TEX_CARRIED` long (the carry of
  `runtime/gfx/gfx_shader.c`) is a pool array too: left in its own set, its textures were never bound and the scene
  drew with empty images until the background pass translated it again. It indexes the first `VK_POOL_TEX_CARRIED`
  entries of the larger binding, which Vulkan allows.

### `vk_pool_marks_enabled()`

The texture pool marks.

`NX2SARIA_TEX_DISUSE`, default on; `runtime/vk/vk_texture.c` "disuse" uses them.

The game samples nearly every texture through the bindless pool: a shader reads a handle from memory and indexes the pool
array with it, so the CPU never learns which of the registered textures a frame samples, and every registered texture
counts as in use (`runtime/vk/vk_texture.c` `idle()`). The game keeps the textures of the areas it has loaded
registered, so none of them could go, and their memory, which the driver charges to the system's commit, grew to 6 GiB
in a session. This rewrite gives each module that indexes the texture pool a storage buffer at set 2, binding
`VK_POOL_MARK_BINDING` (a uint per pool slot, `runtime/vk/vk_exec_draw.c` "pool marks") and, after every access chain
into a pool texture array, a store of 1 into the slot it indexes. The stores of one warp to one slot are one memory
transaction. The replay copies the buffer out and clears it every few submissions; slots marked mean textures sampled. A
module that indexes the pool and cannot be marked (SPIR-V before 1.3 or after 1.6, a stage whose stores the device does
not allow, no memory) is counted in `vk_pool_marks_missed`, which keeps `runtime/vk/vk_texture.c` from evicting by
disuse: an unmarked shader could sample a texture the marks call unused. `make_module()` logs the first few by stage,
version, size, hash and reason.

`NX2SARIA_TEX_DISUSE` 2 is marks and stand-ins with nothing freed (`runtime/vk/vk_texture.c` "disuse"). Whether a
stage may store comes from the GPU's features (`vertexPipelineStoresAndAtomics`, `fragmentStoresAndAtomics`):
`runtime/vk/vk_device.c` enables every feature the GPU reports.

### `spirv_pool_mark()`

A fragment shader that stores to memory runs before the depth test unless it declares EarlyFragmentTests: the GPU must
run it for fragments the test then rejects, as the stores are visible. The marks would so take the early depth rejection
from every marked fragment shader and make overdraw cost full shading. With `early_ok`, a fragment shader whose result
is the same under early tests gets EarlyFragmentTests along with its marks: one that discards, writes its depth, sample
mask or stencil reference, or already writes memory does not, as early tests would change what it does. The caller
passes `early_ok` only for a pipeline without alpha to coverage, which also drops samples after the shader
(`ensure_init()` keeps alpha to coverage out of the dynamic state while marks are on, so a pipeline knows). A fragment
shader's marks are stored where it ends, not where it samples. A store between a sample's coordinates and the sample
takes the quad's helper lanes out of the code in between on AMD's driver for Polaris: their coordinates stayed stale,
the derivatives at a triangle's edge were garbage, and edge pixels came out dark (the lines along the paths' triangles
since 0.7.0). Each access only notes its slot in a Private variable (a register); before each return of the entry
function, and before a kill, terminate or demote anywhere, every variable is stored to its slot, a variable no access
set going to the spare slot past the pool (`runtime/vk/vk_exec_draw.c` `pool_marks_make()`). An access in a loop or
outside the entry function can run more than once with other slots: it notes them in a ring of RING Private variables,
so a loop's last RING slots are all marked. `*status`: 1 marked, 0 no pool texture access to mark, -1 one that could not
be marked (`*why` says what stopped it). NULL unless marked.

What it reads before the functions:

- `bind[id]` is the id's Binding + 1, and `set2[id]` says its DescriptorSet is 2. Pool texture variables are those at
  set 2 with a binding below `VK_POOL_KINDS` (`spirv_pool_rebind()`); a variable already at the marks binding means the
  module was marked before, and nothing is done.
- `em_end` is past the entry points and execution modes; `rtype[id]` an id's result type, where the deferred path
  needs it (`is_type[]`: the ids of OpTypeVoid .. OpTypeForwardPointer).
- The stage: execution models up to 3 (Vertex, TessellationControl, TessellationEvaluation, Geometry) need
  `vertexPipelineStoresAndAtomics`, a fragment stage `fragmentStoresAndAtomics`.
- `early` is given up when the module has EarlyFragmentTests already, and when it declares storage buffers of the
  guest's own: its stores would change under early tests.

The sites. `site_def` has a byte per pool access (an access chain is 5 words or more, which bounds their number): 0
stored in place; else deferred, bit 1 the index is signed (a bitcast), bit 2 a ring (in a loop or outside the entry
function). Only a fragment module with one entry point defers.

`NX2SARIA_POOL_MARK_SHARED` (default 1): the module's ring sites share one ring and its count, and an access notes its
slot only when it differs from the slot noted just before it (consecutive repeats take no entry). With a ring per
site, a shader with many pool accesses in a loop (the castle moat's water: 69 shadow taps of one light's map per
iteration) carried 69 rings of 8 dynamically indexed words and stored up to 552 marks per fragment where it ended: the
water's draw took 7.4 ms per frame on an RTX 5080, 0.3 ms without the marks. The shared ring keeps the last RING runs
of slots, which for such a loop is one per light, where a site's ring held its last RING accesses, all the same slot.
It is not the union of what per-site rings held: an access order can push out a slot that a site's own ring kept. So a
shader that noted more than RING runs sets the overflow word at its exits (`VK_POOL_MARK_OVERFLOW`), and the read that
sees it counts every slot as sampled, for every use of the marks (`runtime/vk/vk_texture.c` `vk_pool_marks_read()`).

`stores` is what each way out stores: a variable per deferred site, or a ring's entries in a loop. With no way out to
store at, or when the code would grow too much (`exits * (deferred + 3 * rings)` over 8192), nothing is deferred and
every site stores in place.

The output:

- From SPIR-V 1.4 on an entry point lists every global variable its functions use, not only the inputs and outputs (the
  translator's mesh shaders are 1.4): each entry point gets the marks variable and the Private variables, a word each.
- Its size: the mode, a word per entry point, decorations, at most 30 words of new types and the variable, 9 a site;
  deferred: a word per entry point and variable, 80 words of types and constants, then per site its variables and the
  store of its slot (ring: 10 + 29), and per way out 29 words a variable and 8 \* 29 a ring (its loop is 83).
- The header up to the annotations' end is copied with the entry points widened and the mode added; then the marks
  buffer's decorations and types, and for the deferred path the types, constants and variables (`pv0`: one per site;
  `ra0`, `rc0`: a ring and its count per ring).
- In the functions (`cur` is the label of the block being copied), before each way out: for each deferred variable,
  Marks[the slot, or the spare when none was noted] = 1; for each ring, first the overflow word when more slots were
  noted than the ring holds, else the spare (`VK_POOL_MARK_OVERFLOW`), then its entries. A demote to helper is not the
  block's end, so every entry is stored, without a loop. At a return, kill or terminate the block ends, so it can end
  in a loop over the entries noted: for (j = 0; j < min(count, RING); j++) mark `ring[j]`.
- After each pool access: a deferred site notes its slot for the stores at the end. On the shared ring a slot other
  than the last noted goes in, and the count goes up by one; on a site's own ring, ring[count % RING] = slot and the
  count goes up by one. A site stored in place gets `marks[slot]` = 1, slot being the pool index of this access.

### `spirv_mark_list_test()`

`--selftest` with `NX2SARIA_SPV_MARK_TEST`=`<list>`: each line "<in.spv> <out.spv>"; the module is moved to the pools
(`spirv_pool_rebind()`) and marked, and written to `<out>` when marked, for spirv-val. Prints the counts; 0 failures
unless a file cannot be read or written.

### `make_module()`

The shader module of a translation, after the rewrites, in this order: `spirv_fixup()`, `spirv_pool_rebind()` and, with
the marks on, `spirv_pool_mark()`, `spirv_reconverge()`, `spirv_nan_preserve()`, `spirv_nan_bits()`,
`spirv_mesh_invariant()`. `early_ok`: a fragment shader whose pipeline has no alpha to coverage (`spirv_pool_mark()`).

- The log of a module the marks cannot take (the first 8) gives a hash: FNV-1a of the words `make_module()` got
  (`orig_w`), as `vk_dump` hashes a shader's SPIR-V; it differs from the translation's only where the pipeline patched
  the module first (added vertex outputs).
- `mm_hash`: FNV-1a of the module `make_module()` last created on this thread, after the rewrites
  (`NX2SARIA_PIPE_FEEDBACK` 2).

### `mesh_libs()`

1 when a mesh pipeline entered now is linked from stage libraries (see `mesh_lib_init()`): it then gets a
stage-independent layout (`pipe_miss()`), which is how the rest of the file tells (`no_libs()`).

### `no_libs()`

1 for a mesh pipeline of layout `l` that has no stage libraries and so no fast link: every build of it is its whole build.
A vertex pipeline is never one (`gpl_link()` itself refuses the shared layout the probe can give it).

### `origin_name()`

Where a shader's translation came from (`gfx_shader_origin`), for the log.

### `crash_note()`

The shaders of the pipeline this thread builds, for the crash notes (`runtime/platform/platform.h`
`plat_crash_context()`): the first 8 bytes of each cache key, as `runtime/gfx/gfx_shader.c` names its cache files.

### `binding_span()`

One past the highest binding a shader uses (1 with no resources: binding 0 is the support buffer).

### `add_stage()`

Adds a stage's resources to the binding list (`Bindings`: the Vulkan bindings in `b`, what feeds each in `pb`), with
bindings b >= 1 moved by `off`; binding 0 (support buffer) is shared. A resource of the texture or sampler pool is not
in the list: it is in set 2 (`spirv_pool_rebind()`), bound by the replay from the pools, and `Bindings.pool` says a
stage indexes one.

### `module_binding_missing()`

The first binding a module's resource variables use that the binding list b[0..n) lacks (module bindings >= 1 moved by
`off`), or -1. A reflection that misses a resource would otherwise hand the driver an invalid layout. The module's Binding
decorations come from the list the shader keeps (`gfx_shader_bindings`), not from its SPIR-V: this runs on the drawing
thread at every pipeline miss (`pipe_miss()`), where a compressed module would be decoded.

A pool array is skipped: it is in set 2, not in this layout (`spirv_pool_rebind()`). Its binding is compared on the low
16 bits, as `GfxShaderRes.binding` is 16 bits.

### `layout_covers()`

Checks both stages of a graphics pipeline against its layout; 0 with `why` filled when one uses a missing binding.

### `make_tpl_bp()`, `make_tpl()`

The update template of a set whose bindings are b[0..n): one entry per binding, in order, over the packed array the
replay fills from the set's first descriptor, a `VkDescSlot` per descriptor. The bind point is the one given
(`make_tpl_bp()`, for an emulated mesh layout) or follows the stages (`make_tpl()`; compute
layouts hold the compute stage only). `VK_NULL_HANDLE` when templates are off, for an empty set, for a push set without
`vkCmdPushDescriptorSetWithTemplateKHR` or when creation fails: the replay then writes the set with write structs.

### `bindings_sort()`

Sorts a binding list by binding (an insertion sort; the lists are short) and hashes it into `*h`; the descriptor total,
or -1 when a binding repeats.

### `layout_get()`

Finds or creates the layout of a binding list (sorted here); NULL on failure. Caller holds `P.lock`. The pipeline
layout has three sets: the list's, an empty one, and at set 2 the pools.

### `mesh_emu_on()`, `layout_get_emu()`

`NX2SARIA_MESH_EMULATION` (GPUs without `VK_EXT_mesh_shader`), decided with the mesh fallback at device creation
(`runtime/vk/vk_mesh_path.h`).

`layout_get_emu()`: the layout of an emulated mesh pipeline. The task and mesh stages' bindings are visible to the
compute stage (they run as compute pipelines of this layout), the fragment stage's stay; push constants
(`MESH_EMU_PC_BYTES`) for the compute stages and the generated vertex shader; set 0 gets a template per bind point.
Caller holds `P.lock`.

### `emu_get()`, `emu_find()`, `vk_pipeline_mesh_emu()`

`MeshEmuEnt`: the compute pipelines of an emulated mesh pipeline, per (mesh, task, layout): made by the pipeline's build
(`create_graphics()`, any thread) and found by its draws. `ok` 0: the stages could not be converted or built.

Each draw of an emulated mesh pipeline looks its entry up (`vk_pipeline_mesh_emu()`), so the lookup takes no lock: an
entry is complete before it is published at its bucket's head (release) and is never removed. `emu_lock` only orders the
publishing. The build (two compute pipelines, up to seconds each in the driver on a first run) is done outside it: held
across the build, as before, it stopped every draw of the replay that looked up any entry meanwhile, 2.5 s of the replay
thread's samples in a first 0.8.0 run in Kokiri Forest on an RX 580 (2026-11-11).

In the entry's layout the task stage's bindings follow the fragment stage's (`toff`). When another build of the same
stages published its entry first, that one is kept and this build's pipelines are destroyed.

### `layout_pair()`

Independent layouts (`P.indep`): the vertex stage's bindings in set 0, the fragment stage's in set 1 (empty without a
fragment shader), each with its own support buffer at binding 0, the pipeline layout created with
`VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT` so stage libraries can use it. Only the `P.push_stage` set is a
push-descriptor set (a layout holds one), when it fits `maxPushDescriptors`; the other is allocated per draw from the
command buffer's pools. On success `*B` holds both lists, set 0 first, `VkPipeBinding.set` filled in. Caller holds
`P.lock`.

### `vs_outs()`

Vertex outputs with independent layouts: each vertex shader declares the union of the inputs of every fragment shader
met with it (`pipeline-keys.bin` at the prewarm, then each pipeline built), so its module does not depend on the
fragment shader of one pipeline. Unwritten ones read (0, 0, 0, 1) as before. `NX2SARIA_VS_OUTS=all` declares all 32.

`vs_outs()` adds `fs`'s inputs (packed ones excepted) to `vs`'s union and returns the union, types in `io` (the first
type met wins; of one type, the widest). When the table (`VO`, `VS_OUTS_CAP` shaders) is three quarters full, a vertex
shader not in it gets the outputs of this pair alone, logged once.

### `mesh_outs()`

What a mesh module writes (`spirv_mesh_outputs()`), kept per mesh shader: the library keys of a mesh pipeline need it at
every lookup (`lib_keys()`), and reading it walks the module's declarations with two tables of its id bound.

- `const uint32_t *w = gfx_shader_spirv_pin(ms, &n)`: once per mesh shader: decoded here if it is compressed.
- `mask = w ? spirv_mesh_outputs(w, n, points) : ~0u`: as `create_graphics()` takes a module it cannot read.
- `if (!MSO.ms[j] && MSO.n + 1 < MESH_OUTS_CAP * 3 / 4)`: a full table: read again next time.

### `mesh_fill()`

The inputs of fragment shader fs that mesh shader ms does not write, which the fragment module of their pipeline gets
filled in (`spirv_fs_fill_inputs()`; `create_graphics()` works the same mask out for a whole build), and in `*points`
whether they are filled as a point sprite's. 0 and no points when nothing is filled.

### `pipe_add()`

Stores a result (a failed one too, so it is not retried every draw). Caller holds `P.lock`.

### The graphics pipeline key

`GKey` is normalized: zero where the state is dynamic or unused.
- `const GfxShader *vs, *fs, *ts`: ts: the task stage of a mesh draw.
- `uint32_t color[8], depth, stencil`: `VkFormat`.
- `uint8_t clamp, poly_mode, a2c, logic_op`: `logic_op`: `VkLogicOp` + 1 when enabled.
- `uint8_t blend_en, dsb, fbl, pad`: `dsb`: an enabled target blends with a SRC1 factor; `fbl`: `VkPipeState.feedback`.
- `uint8_t blend[8][6]`: src, dst, op, src alpha, dst alpha, alpha op (`VkBlendFactor` / `VkBlendOp`).
- `uint8_t rate[17]`: per binding: 0 unused, 1 vertex, 2 instance.
- `uint8_t indep`: `P.indep` when built: the probe draws through both layouts.

### `is_src1()`, `uses_src1()`

SRC1 blend factors read the fragment shader's second output (dual-source blending); the pipeline then needs a fragment
module whose output 1 is Location 0, Index 1 (`spirv_dual_source()`).

`uses_src1()` runs per draw (`gkey_fill()`, `target_wmask()`): only the blend factors of blend-enabled targets are read
until one is SRC1, which the game never sets (0 of 876 pipelines in the field and fight saves).

### `noopt_flag_for()`

`NX2SARIA_VK_NOOPT=1`: pipelines built without the driver's optimiser, for a driver whose shader compiler crashes on one
(the test switch for such a report); slower shaders, the same picture. `mesh`: only pipelines with task and mesh stages,
the terrain's and the foliage's. 0: none.

Unset: `mesh` on AMD's Windows driver for RDNA 1 and 2 (`vk_caps.amd_sc`), 0 elsewhere. That driver loses the device ("No
fault detected", a timeout) minutes into play on RX 6600 to RX 6950 XT cards in every version from 0.6.0 to 0.9.2, with
the pipeline library on or off and the lazy write-back on or off. Where the reports' pass marks name a pass the GPU was
in, twelve logs of five reporters, it is a pass whose first draw is a mesh pipeline (mesh shader ba4302f1 with fragment
shader f64a96dd in nine, the one-draw pass of 1bbdb17e with 6cc3a476 in three). The logs of 0.9.1 and 0.9.2 end with the
game's dispatch of cs d7566eb1 finished and nothing after it marked: the group of draws that followed was recorded by
the replay workers, whose passes have no marks, so which draw of it hung is not known. No reporter ran with the
optimiser off, so that it is the cause is not shown: it costs shader speed in those pipelines and not the picture, and
`NX2SARIA_VK_NOOPT=0` gives the optimised ones back. The flag is part of what the driver's cache keys a pipeline by, so
those pipelines are built once more after the change.

Since then that driver draws mesh programs through the mesh fallback by default (`runtime/vk/vk_mesh_path.h`), and no
mesh pipeline is built there: this applies with `mesh_fallback` = 0 alone, as the 0.10.x way to compare against.

### Pipeline creation feedback

`NX2SARIA_PIPE_FEEDBACK`, default 1; 0 off.

Each pipeline, library and compute build asks the driver whether `pipeline-cache.bin` (`P.cache`) held it
(`VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT`) and how long the create took. The counts appear
with the prewarm, the stage library prebuild and every cache save, so a log tells whether a slow boot compiled or read
its pipelines: on NVIDIA the driver's own disk cache is kept per executable, so a renamed or new exe gets nothing from
it, and only `pipeline-cache.bin` can make its first boot quick. =2 also logs every compute build with a hash of its
final module, and every graphics pipeline build with the hashes of its final modules and of its fixed state, so two runs
can be compared for a module or a state that changes between them.

### `fb_chain()`

Chains `fb` into a create info's `pNext` (`next`: its current `pNext`). Returns what the create info's `pNext` becomes.

### `fb_log()`

One line: per kind, builds that came from `pipeline-cache.bin` of all builds so far, with the average time of each.

### `target_wmask()`

Colour write mask of target `i`: NVN's channel mask, zero for a target outside the fragment shader's output map (SPH
omap). Maxwell leaves such a target untouched; Vulkan writes undefined values, and on AMD they lit grass in front of
Link as flat lime. Zeros are wrong too: the game's half-res transparency pass binds its linear depth (R32F) as target 7,
samples it in the same draws and writes only targets 0-2, 4 and 5; zeroed, the depth that later passes rebuild from it
was 0 in tiles, and the fog over distant geometry showed rectangles of clear sky.

The same holds per component: a shader that writes a target's RGB and not its alpha leaves the alpha alone on Maxwell,
while Vulkan writes undefined values, and AMD's driver writes whatever its export registers held (NaN and saturated
pixels the bloom then spreads). Components outside the shader's omap are masked out, where it is known.

Target 1 gets mask 0 when the pipeline blends with a SRC1 factor: output 1 is then the second blend source
(`spirv_dual_source()`).

### `gkey_fill()`

A mesh draw has no vertex input in its key, and its topology class is the triangle one, as the mesh stage declares its
primitives, or the point one for a draw of points (emulated point meshes draw point lists).

### `vs_union_missing()`

Independent layouts: the outputs the vertex shader lacks from its union with `s->fs`'s inputs added (`vs_outs()`), types
in `fin`. The vertex module (and its pre-rasterization library) depends on the vertex shader and this mask only. A
location another fragment shader reads as another type takes this pair's type (rare, logged once).

### Water corrections

[water-projective.md](../water-projective.md); `NX2SARIA_WATER_PROJECTIVE`, default 1; 0: the translations as built.

Two of the game's water translations are built from an edited module (`runtime/gfx/spirv_water.c`):

- The coordinate pair (mode 1). The vertex stage worked out each vertex's place on the screen (x / w and y / w, scaled
  to 0..1) and the fragment stage read it interpolated without perspective, to sample the scene behind the water. A
  triangle with a corner behind the camera has no such place for that corner, and the scene was sampled elsewhere: a
  rectangle of water over solid ground at the Hyrule Castle moat and the Castle Town canal. The pair now hands x, y and
  w down undivided and the fragment stage divides. Both stages change, or neither.
- The absorption material (mode 2): its distance through the water could come out negative, which turned the dimming
  into a gain (a red beam). The distance stored is at least 0.

The translations are known by the FNV-1a of their SPIR-V, one profile per translator output (the host options of AMD's
and NVIDIA's drivers give different modules); every other module is built as it is. The edited modules are made at their
first use and kept. Modes go into the stage libraries' keys (PrKey, FsKey), so a library of an edited module is never
linked with one of the original.

`WaterProfile`:

- `uint64_t vs, fs, fs_set1`: the coordinate pair: vertex, fragment, the fragment in its set 1 form.
- `uint64_t mat, mat_set1`: the absorption material, and in its set 1 form.
- `uint32_t mat_pointer, mat_distance`: the ids of the store its distance goes through.

`water_mods`: the edited modules, by the hash of the module each stands for as it is built: a vertex translation, or a
fragment module in its set 1 form. `state`: 0 empty, 1 made, -1 the edit refused the module (it is built as it is).

### `water_module()`

The edited module that stands for the module of hash `key`, or NULL. `src`: the translation it is made from at the first
call (NULL: only look), whose SPIR-V is pinned for the edit only; a fragment translation is first put into its set 1
form, which must be the module `key` names, as it is the form the pipeline builds.

### `water_projective_pair()`

The water mode of a pipeline's shaders: 1 a coordinate pair whose two edited modules exist, 2 the absorption material
whose edited module exists, else 0. The drawing thread asks it for every pipeline it links (`lib_keys()`): the modules'
hashes are the ones the shaders keep (`gfx_shader_spirv_fnv`), and the SPIR-V is read only to make an edited module,
once per profile. The absorption correction is independent of the coordinate pair.

### `water_pair_module()`

The fragment module of a pipeline in water mode `mode` (`water_mode()`): the edited one for the module as it is built
(in its set 1 form), else the module itself.

### `water_pair_vertex()`

The vertex stage of a coordinate pair (mode 1): the edited module for the translation itself, taken before the outputs
the vertex shader's union adds (`spirv_add_outputs()`), which differ from one install to the next and are added to the
edited module as to any other. NULL in every other case. The module stays the table's.

### `water_mode()`

`water_projective_pair()` for a pipeline of key `k`. A coordinate pair blended with dual-source factors keeps its original
modules: `spirv_dual_source()` changes the fragment module before its set 1 form, which is then none the table holds.

### Line width

`NX2SARIA_LINE_WIDTH`, default 1; 0: every line 1 pixel wide, as before.

The game sets a width for its lines (`nvnCommandBufferSetLineWidth`, recorded as `OP_BIND_STATE` kind 7): 4.5 pixels for
two line draws a frame into a 144x144 R8 target, which were drawn 1 pixel wide. A pipeline of the line topology class
lists the width as dynamic state, in its whole build and in its pre-rasterization library (`PrKey.lines`), and each draw
of lines records the width the game set, held to the device's range. Pipelines of the other classes are built as before,
so their entries in the pipeline caches and the driver's stay the ones in use. A polygon mode that draws triangles as
lines keeps 1 pixel: the mode can be dynamic state, which no pipeline key tells apart.

### `vk_pipeline_line_width()`

Sets the width of a draw of lines, rounded to the device's step and held to its range. The log has one line for each
width in a row: the game uses one or two.

### `task_off()`

What a mesh pipeline's task stage bindings are moved by in layout `l`: past the mesh stage's in a stage-independent
layout, where the two share set 0 (`mesh_pair_layout()`), and past the fragment stage's in the shared set
(`pipe_miss()`).

### `mesh_dyn()`

The dynamic states of a mesh pipeline into `out` (room for `P.n_dyn` + 1): `P.dyn` without the vertex input, its strides,
the topology and primitive restart, which a pipeline with a mesh stage must not list, and with the fragment shading rate
where the device sets one per draw (`runtime/vk/vk_exec_draw.c` `mesh_shading_rate()`). Returns their number.

### `create_graphics()`

The whole build of a graphics pipeline, in this order:

1. The stages' translations are pinned until the pipeline is made (`runtime/gfx/gfx.h` `gfx_shader_spirv_pin()`):
   `vcode` and `fcode` become patched copies on the way, `vplain` and `fplain` stay the translations.
2. The water mode (`water_mode()`), 0 for a mesh pipeline and in a shared layout: the edited fragment modules are in
   their set 1 form. The water pair's vertex edit is for a translated vertex stage: an emulated mesh draw's generated
   one has none.
3. `NX2SARIA_MESH_EMULATION` (`emu`): the task and mesh stages as compute pipelines (`emu_get()`), a generated vertex
   shader here; it is a vertex pipeline from there on (`mesh` 0).
4. A mesh pipeline's task module, its bindings following another stage's in its set (`task_off()`).
5. The vertex outputs. With independent layouts they come from the vertex shader's union, not from this fragment
   shader (`vs_union_missing()`); in a shared layout, from this fragment shader's inputs. A mesh stage's outputs are
   read from its module (`spirv_mesh_outputs()`), as the translation reflects none; an emulated one (`mesh_emu_vs()`)
   writes what the fragment stage reads.
6. The fragment module: inputs no mesh output feeds read as Maxwell gives them (`spirv_fs_fill_inputs()`), then the
   dual-source rewrite, then its set or its bindings, then the water edit and the marks (`water_pair_module()`).
7. The stages. `pin_subgroup32()` reads `vplain` and `fplain`: `vcode` may be a patched copy that is freed by then.
8. The state from the key. A bound colour target outside the shader's output map has write mask 0
   (`target_wmask()`). NVN's default provoking vertex is the last (SetProvokingVertexMode, `runtime/nvn/nvn_ext.c`);
   Vulkan's is the first. Without dynamic vertex input the vertex layout is pipeline state (zero in a mesh pipeline's
   key). Lines take the game's width per draw (`key_lines()`, `vk_pipeline_line_width()`); a mesh pipeline lists
   `mesh_dyn()`'s states.
9. The create. A compiler that gives up on a pipeline (AMD's Windows driver answers `VK_ERROR_UNKNOWN`) may still build
   it without its optimiser: one more try that way, slower shaders for that pipeline instead of a draw left out.
   `PERF_PIPE` counts on the render thread only, so background builds stay out of it.
10. With `NX2SARIA_PIPE_FEEDBACK` 2, one line with the hashes of the final modules (`mm_hash`) and the state hash: what
    the create info takes from the key besides the modules.

### Graphics pipeline library

`vk_caps.gpl`.

A pipeline is linked from four libraries, each cached by the part of the pipeline key it depends on: vertex input
(topology class, vertex attributes without dynamic vertex input), pre-rasterization (vertex shader, the outputs its
union adds, static rasterization state), fragment shader (fragment shader, dual-source rewrite, samples) and fragment
output (targets, blend, samples). With independent layouts each stage's set depends on its shader alone, so the
pre-rasterization library uses {set 0, null} and the fragment shader library {null, set 1}. The fast link runs on the
thread of the draw that needs it (no link-time optimisation); the optimised pipeline is the job `pipe_miss()` queued (a
monolithic build with the same modules), which replaces the fast one in the Pipe when done. Libraries and fast pipelines
live until `vk_pipeline_shutdown()`. No library is made to be optimised at its link: the optimised pipeline is that
whole build, so the links take what the libraries hold as it is.

A mesh pipeline is linked from three of them, its pre-rasterization library holding the task and mesh stages
(`mesh_lib_init()`).

Background build signalling, all with `P.lock`: `work_cv` wakes the pipeline workers, `done_cv` wakes threads waiting
for a pipeline or a stage library to be published, and `lib_cv` wakes the library workers (`lib_worker()`), as
`work_cv`'s signal must reach a pool worker.

`libs` (`Lib`): `pre`: built by the boot prebuild (`lb_start()`); `used`: a draw's link found it; `building`: a thread
builds it or `job` holds it queued (`pipe` not set yet); entries live until `libs_destroy()`.

`PrKey`:

- `uint8_t lines`: `key_lines()`: the library lists the line width as dynamic state.
- `const GfxShader *ts`: a mesh pipeline's library (`vs` is its mesh stage): its task stage, or NULL.

`FsKey`: `fill`, `points`, `vrs`: zero for a vertex pipeline. A mesh pipeline's: the inputs filled in the module and whether
as a point sprite's (`mesh_fill()`), and 1 when the library lists the fragment shading rate as dynamic state
(`mesh_dyn()`).

`FoKey`: `fbl`: `GKey.fbl`, the feedback loop flags of the library (`gpl_feedback()`).

### `lib_create()`

Builds one library from the pipeline's state; `key` is the library's key (`PrKey`: `fin` holds the added outputs). The
shaders whose SPIR-V it reads stay pinned until it returns.

- **Dynamic state.** A mesh pipeline's pre-rasterization library lists what a whole mesh pipeline lists, and a
  library of lines the line width: the rasterization state is this library's, as in `create_graphics()`. The shading
  rate is part of the pre-rasterization and of the fragment shader state: a mesh pipeline's libraries of both kinds
  list it as dynamic state, as a state dynamic in one library has to be in every other library that holds it. Such a
  library is a mesh pipeline's alone, so it takes that pipeline's list.
- **A mesh pipeline's pre-rasterization stages** (mesh, then task) as `create_graphics()` builds them: the mesh module
  as translated, the task module with its bindings moved past the mesh stage's (`task_off()` in a stage-independent
  layout); the mesh stage takes `pin_subgroup32()` and both take `stage_sg()`'s requirements there, so here too. No
  vertex outputs are added: what a fragment shader reads and the mesh stage does not write is filled in the fragment
  module (`FsKey.fill`), before the other rewrites, as in `create_graphics()`.
- **Feedback loops.** Whether the pipeline may sample an attachment it renders to is part of the fragment output state:
  the flags go on this library alone, and a pipeline linked from it has them (`gpl_feedback()`).
- **Creation feedback** is noted for the shader libraries only: the others compile nothing.

### `lib_find()`

The library entry for `key`, NULL when absent. Caller holds `P.lock`.

### `lib_put()`

Enters a library built with `P.lock` released; another thread's entry made meanwhile wins and `pipe` is destroyed.
Returns the entered handle, `VK_NULL_HANDLE` when another thread's build is still to publish the entry. Caller holds
`P.lock`.

### `lib_new()`

A building entry for `key`, NULL when out of memory. Caller holds `P.lock` and found none.

### Library jobs

`LibJob`: shader libraries a waiting draw needs (`lib_wait()`, `link_queue()`): queued in `U` ahead of every pipeline
job, so an idle worker builds each at once and the vertex and fragment libraries of one draw build in parallel. A job no
worker took is built by the first thread that needs its library (`lib_get()`; `lib_wait()` only without library
workers). A job with `p` is a waiting draw's fast link of `p` (after its library jobs), run on a worker so the drawing
thread only waits. Under `P.lock`.

`U`:

- `int idle`: workers asleep on `work_cv`.
- `int running`: jobs workers are building.
- `int on`: `NX2SARIA_PIPE_WAIT`: 1 workers (default), 0 the drawing thread builds them (draw); set by
  `vk_pipeline_async_enable()`, before the replay runs.
- `int lib_workers`: workers that take only these jobs (`lib_worker()`), started with the pool.

### `lib_build()`

Builds `b` (building, not queued) with `P.lock` released and publishes it; a failed build is entered as
`VK_NULL_HANDLE`. A mesh pipeline's shader libraries are also counted apart (`P.g.m_lib_*`). A shader library that took
25 ms or more was not in the driver's cache: the log says why not, for the first 32 of a run.

### `lib_steal()`

Takes `b`'s queued job out of the queue and builds it on this thread. Caller holds `P.lock`.

### `lib_get()`

The cached library for `key`. Caller holds `P.lock`. `build` 0: lookup only (`VK_NULL_HANDLE` and `*miss` when absent
or still building); `build` 1: a missing one is built with `P.lock` released (a failed build is cached too).

An entry that is building is waited for, or, when its job is still queued, built here (`lib_steal()`). A new entry is
entered first (`lib_new()`), so other threads wait for this build instead of repeating it; only when there is no memory
for the entry is the library built without one, and then another thread may have built it meanwhile.

### `lib_keys()`

The four library keys of pipeline `s` (key `k`). A mesh pipeline (one with stage libraries: `no_libs()` is 0) has no use for
the first, and what tells its two shader libraries from a vertex pipeline's is here: the pre-rasterization key names the
mesh stage and the task stage and adds no outputs (a mesh shader is in no vertex output union), the fragment key carries
what is filled in the module and the dynamic shading rate (`FsKey`). Neither takes a water correction, as
`create_graphics()` gives a mesh pipeline none.

### `gpl_feedback()`

`NX2SARIA_GPL_FEEDBACK` (default 1; 0 = as before): a pipeline whose fragment shader samples an attachment of its own
pass (`VkPipeState.feedback`: soft particles and water reading the depth buffer, decals, full-screen passes that read
the target they write) gets a fast link like any other. The feedback loop flags belong to the fragment output state, so
they are part of that library's key (`FoKey.fbl`) and of nothing else; the shader libraries are the ones the same
shaders use without a loop. Before, such a pipeline had no fast link at all and its draws waited for, or were skipped
during, the whole optimised build. On a first visit to Kokiri Forest (RTX 5090, every stage library in the driver's
cache, DLSS Frame Generation on, so no frame is held back) full-screen draws of such pipelines waited 267 ms at the
area's first frame and 410 ms in the middle of a walk; linked, 3 ms.

### `gpl_link()`

A fast-linked pipeline for `s` from its stage libraries. Caller holds `P.lock`. `build` 0 (drawing thread): links only when
the shader libraries exist (vertex input and fragment output libraries, which hold no shader, are built here); `*final`
0 = not yet, a worker builds them. `build` 1 (worker): builds the missing libraries and links with `P.lock` released.
`VK_NULL_HANDLE` with `*final` 1 when GPL cannot express the pipeline (shared layout, which is also a mesh pipeline's
without stage libraries; a feedback loop with `NX2SARIA_GPL_FEEDBACK=0`) or a build failed.

A mesh pipeline is linked from three libraries: the pre-rasterization one with its task and mesh stages, the fragment
shader one and the fragment output one. It has no vertex input state, and a complete pipeline needs that state only
where its pre-rasterization state holds a vertex shader. The link names no state of its own and no dynamic state: what
is dynamic is what the libraries were made with.

- The shader libraries are asked for first: without them there is no work to do.
- The crash note names the parts that differ between links, so a link that crashes the driver names its case.
- `P.g.link_ms` is the link alone: library builds are counted apart.

### Background graphics pipeline builds

`vk_pipeline_async_enable()`.

A miss in `vk_pipeline_graphics_async()` enters the Pipe as building and queues a job; the replay skips draws with it
until a worker publishes the handle. The job holds copies of the state and the key. What they point to outlives it:
shaders (`gfx_shader_get` results) live for the process (`gfx_shader_drop_memory` is the shader probe's, which builds no
pipelines), layouts until `vk_pipeline_shutdown()`, which drains the queue first. Workers call
`vkCreateGraphicsPipelines` without `P.lock`: `P.cache` is created without
`VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`, so the driver synchronises builds into it; only the copy of its
data in `cache_save()` is kept apart from them (`cache_rw`). Compute pipelines stay synchronous: a skipped dispatch can
change what the game computes.

Two queues: draws' misses (`A.head`) and the boot prebuild (`A.low`, "Pipeline keys"). Workers take the low
queue only when the other is empty and run its jobs at `PLAT_PRIO_BELOW_NORMAL`, so guest threads win. A draw that finds
a prebuild job still queued moves it to the draw queue; one that finds it running raises its worker.

`Job`:

- `struct Job *next, *prev`: `prev`: low queue only.
- `uint8_t low`: in the low queue, or running at below-normal priority.
- `uint8_t prebuild, demanded`: from the key file; a draw asked for it before it was ready.
- `uint8_t rested`: counted as held back by the play gate (`low_held()`).
- `int worker`: running on `A.thread[worker]`; -1 queued; `JOB_ON_DRAW` on the thread that waits for it.

`A`:

- `int on`: set once by `vk_pipeline_async_enable()`, before the replay runs.
- `uint8_t prio_low[MAX_WORKERS]`: worker runs below normal (under `P.lock`).
- `Job *head, *tail`: draws' misses.
- `Job *low, *low_tail`: prebuild.
- `uint32_t inflight`: queued or running, both queues.
- `int low_running`: pool workers running a job taken from the low queue.
- `int gate_shut`: pool workers the memory gate has refused a low job since it last admitted one.
- `uint64_t first, taken`: waited-for jobs moved to the head of the draw queue; built by the waiting thread.
- `PlatCond idle`: with `P.lock`: no job queued or running.

### Pipeline keys

`pipeline-keys.bin` next to the pipeline cache.

Every graphics pipeline built in a game run appends a KeyRec: the `VkPipeState` it was built from with the shader
pointers zeroed and the shaders named by their `gfx_shader` input ids instead, which do not depend on the translator, so
the record rebuilds the pipeline without guest memory. At the next boot `vk_pipeline_prewarm()` (`rt_boot`, before the
guest runs; `NX2SARIA_PIPE_PREBUILD=0` skips it) loads each record's shaders, translating the ones not cached under the
current translator from their stored inputs (`gfx_shader_prewarm`; a record whose shader has neither is dropped), enters
each Pipe as building under the key a draw would compute, queues it to the low queue and builds the queue on every core.
Records are deduplicated by a hash of their bytes; they are collected under `P.lock` by the worker that built the
pipeline and appended by the present thread (`vk_pipeline_autosave()`) or at exit (`vk_pipeline_save()`). A file with
another magic, version or record size is ignored and rewritten; a version 3 file (no task stage) is converted and
rewritten. Bump `KEYS_VERSION` when `VkPipeState` or the record changes meaning without changing size.

- `#define KEYS_VERSION 4`: 3: shaders named by input id (`runtime/gfx/gfx_shader.c`); 4: mesh pipelines, with their
  task stage.

`KeyRec`:

- `uint8_t vs[32], fs[32], ts[32]`: `gfx_shader_input_id` of each stage (vs: the mesh stage of a mesh pipeline); fs all
  zero with `has_fs` 0, ts with `has_ts` 0.
- `VkPipeState s`: vs, fs, ts zero.

`K`:

- `int header_ok`: the file exists with our header: append.
- `uint64_t *seen`: open addressing set of record hashes (0 empty).
- `KeyRec *pend`: records not written yet.
- `uint8_t *file`: the file as read at boot, until the prewarm is done with it.
- `size_t file_n`: records in it.
- `size_t own_n`: of them, the first ones: this install's own (`keys_open()`); the shared list's follow.
- `PlatMutex io`: one writer.
- The fields from `loading` on are the prebuild's progress (under `P.lock`).
- `int loading`: the prewarm still queues records.
- `int warm`: `vk_pipeline_prewarm()` owns the file (`prebuild_start()` leaves it).
- `uint32_t left`: prebuild jobs queued or running.
- `uint32_t gate_ended`: boot builders the memory gate ended (`warm_builder()`).
- `double t0`: `now_ms()` when the prebuild started.
- `int prep`: `t0` is the preparation's start (`seed_pipelines()`), not the ready device's.
- `double until`: `now_ms()` after which the boot builders stop (`prewarm_run()`); 0 none.

### `seen_add()`

1 if `h` was new. Caller holds `P.lock`.

### `rec_fill()`

The record of a state. Primitive restart is dynamic state, the same pipeline either way, so the record holds 0.

### `keys_note()`

A pipeline was built for a draw: queue its record for the file. Caller holds `P.lock`.

Not recorded, as a stage without an input id cannot be loaded again: a pipeline with one of the runtime's own shaders
(`gfx_shader_own()`), and the mesh fallback's vertex stage (`runtime/vk/vk_exec_draw.c` `mfb_vs_get()`).

### `keys_flush()`

Appends the pending records (present thread or exit; never the render thread). The file is synced, so appended records
are on the disk before the next append.

### `seen_has()`

Caller holds `P.lock`.

### `keys_open_seed()`

The shared pipeline list as recorded pipelines (`NX2SARIA_PIPE_SEED`, default 1; 0: this install's own records only).
`seed-pipeline-keys.bin` next to the game holds the list's records (`tools/prepare_shaders.py` writes it). Before, the
boot built only what this install drew, so a new install met every pipeline in play: in Hyrule Field one 600-frame
stretch of a first visit had 1,769 draws wait 461 ms in all for their pipelines, the longest frame 88 ms. The list's
records now follow the install's own in `K.file`, so the prewarm counts their pairs in the vertex output unions (and the
pipelines it builds from them are the ones draws ask for) and builds them as it builds the install's: the boot for at
most `PREWARM_PIPE_MS`, the pool workers the rest. The preparation builds the same list into the caches
(`seed_pipelines()`, `NX2SARIA_PREPARE_PIPELINES=all`), so that boot finds them there. They are not added to the set or
the file: `pipeline-keys.bin` keeps what this install drew, and a pipeline a draw builds that the prewarm did not still
goes into it. A list in another record layout is left out, as the preparation leaves it. The list's records come after
the install's own: those met first in play keep coming first.

### `keys_open_own()`

### `keys_open()`

Reads the key file into `K.file` and its record hashes into the set (`keys_open_own()`), then adds the shared list's
records (`keys_open_seed()`). Caller holds `P.lock` (`ensure_init()`).

### `keys_open_own()`

- `gfx_shader_record_at()` puts `shader-answers.bin` next to the key file (`runtime/gfx/gfx_shader.c`).
- A file whose length is not a whole number of records had a torn append: its whole records are kept and the file is
  rewritten from what was read (`header_ok` 0, the records pending).
- The vertex pipelines of a version 3 file keep their prewarm: each record is converted without a task stage, and the
  file rewritten in this version the same way.

### `ready_note()`

Caller holds `P.lock`. `p`, which a draw missed, has its first usable pipeline: how long that took is how long its draws
were skipped or waited. `fast` 0: it had no fast link and this is the optimised build.

The summary (`save_with()`) counts what a slow one can be: a mesh pipeline (its task and mesh library is the largest to
compile, and without stage libraries, `no_libs()`, this is its whole build), one the key file names whose prebuild had
not come to it, one with a shader translated in this session (nothing of it was prepared, so its libraries compile
cold). The first `READY_LOGS` that took `READY_SLOW_MS` or longer get a line each with those facts, so a log says which
pipelines left their draws out of the picture and for which reason. The mesh pipelines are also counted by themselves:
how many became usable through a fast link, and after how long.

A pipeline goes into `pipeline-keys.bin` once a draw can use it, so at its fast link too: recorded only when its
optimised build had finished (`job_build()`), the pipelines met in the last seconds of a session were lost. A session
of 2026-11-13 met 46 late pipelines in adult Kokiri Forest and ended about 2 s later; 23 of them never reached the
file, and with it the shared pipeline list (`tools/pipeline_list.py` `merge()`). `keys_note()` takes each record once.
`s` is the state the caller built `p` from: a job whose optimised build failed is gone (`job_build()` clears `p->job`
and records nothing) before a queued fast link makes `p` usable, and that link is recorded from `s`.

### `u_run()`

Runs a waiting draw's job taken from `U` on a worker: a library build or, for a job with `p`, `p`'s fast link
(`gpl_link()` with `build` 1 builds a library still missing and waits for one another worker builds). Releases `P.lock`
while building or linking. After the link the pipeline's job only does the optimised build (`job_lower()`); when the
job published a pipeline meanwhile, the link is destroyed.

### `fast_workers()`

`NX2SARIA_PIPE_FAST_WORKERS` (default 1; 0 = as before): the library workers also take the fast link of a draw-queue job
(`fast_take()`). A draw that is skipped rather than waiting leaves its pipeline to the pool, where a job is first
fast-linked and then goes back in the queue for its optimised build. The pool keeps one worker out of optimised builds,
so with the others inside one (about 100 ms each with the driver's cache warm, 350 ms cold) that single worker linked
every new pipeline of a burst, one after another at 5 to 10 ms each. On a first visit to Kokiri Forest (RTX 5090,
libraries in the driver's cache) 180 of the 444 pipelines draws missed took over 50 ms to become usable, the longest 1.3
s, and their draws were missing from every frame until then; with the library workers linking, 23 did, 19 of them mesh
pipelines, which had no fast link then (they have one with `mesh_libs()`).

### `fast_take()`

The first draw-queue job whose pipeline still lacks its fast link, taken out of the queue; NULL when there is none.
Caller holds `P.lock`.

### `lib_worker()`

A worker for stage libraries and fast links only: a waiting draw's jobs (`U`) first, so a draw that waits finds one free
even while the pool builds pipelines and the drawing thread never builds a library itself; then the fast link of a
draw-queue job (`fast_take()`), whose optimised build goes on to the pool.

- `fast_tried` is set before the link, which releases `P.lock`: no other thread links the pipeline meanwhile, and a
  draw that waits for it waits for this link.
- Linked, its job goes to the head of the low queue for the optimised build: ahead of the prebuild, behind other
  draws' misses.
- Without a fast link its job goes back to the head of the draw queue: its whole build is next.

### The memory gate of the background builds

`runtime/vk/vk_build_gate.h`.

Every pipeline built ahead of its first use passes here before it starts: the recorded pipelines the boot builds
(`warm_builder()`), the pool's low queue during play (`pipe_worker()`: the rest of the recorded pipelines and the
optimised builds behind a fast link), the compute pipelines and stage libraries of the boot (`lb_worker()`), the library
warm (`lw_worker()`) and the large compute shaders (`bg_main()`). A build a draw or a dispatch waits for never does: it
is needed now.

`NX2SARIA_BUILD_FREE_MB` (default 4096; 0 no check): the system commit headroom that stays free of them.
`NX2SARIA_BUILD_RESERVE_MB` (default 64): what one build under way is taken to cost the driver's compiler while it runs;
a large compute shader reserves `GATE_BIG_MB`. A build starts only when the headroom holds the part kept free, the
reservations of the builds under way and its own.

Measured on the machine this was written on (RTX 5090, 73,154 MB commit limit, warm driver cache): the boot's 3,898
recorded pipelines on 16 threads raised the process commit by 3,054 MB in 2.2 s, and the 4,066 compute pipelines and
libraries after them, together with the game's own start, by another 9,824 MB. A device was lost on that machine at
56,673 of 65,531 MB system commit right after such a prebuild.

The commit is read at most every `GATE_SAMPLE_MS`: sixteen builders finishing a cached pipeline every half millisecond
would otherwise ask the system thousands of times a second. What the builds took since the last reading is covered by
their reservations.

`GATE_WAIT_MS` (30 s): how long a boot or warm builder waits for room before it leaves the rest to their first use.

`GATE.off`: a preparation run (`seed_pipelines()`): nothing is refused, there is no game to keep room for.

### `gate_enter()`, `gate_leave()`

1 when a build estimated at `est` MB may start, and then `est` is reserved until `gate_leave()`. `est` 0: an ordinary
build (`NX2SARIA_BUILD_RESERVE_MB`), whose estimate `*took` returns for `gate_leave()`.

### `gate_text()`

The gate's numbers for a log line that says why a builder stopped or waits.

### `gate_wait()`

A boot or warm builder's wait for room: 1 with the estimate reserved (`*took`), 0 when `*stop` was set or
`GATE_WAIT_MS` passed without room, which the caller takes as the end of its work. `who` names the builder for the
log's one line a wait.

### The play gate

`runtime/vk/vk_play_gate.h`, where the reasons are.

`NX2SARIA_BG_BUILD_PLAY_DRAWS` (default 600; 0: off): builds made ahead of their first use rest while the frame last
shown had this many draws or more and was shown within `PLAY_GATE_SHOWN_MS`. Those are the pool's low queue
(`pipe_worker()`) when stage libraries are on, and the library warm (`lw_worker()`). A pipeline a draw waits for never
rests: the draw queue and the libraries of waiting draws are not gated, and a draw that needs a low job moves it to the
draw queue (promote), which wakes a resting worker and gets the job's fast link first. Without stage libraries that draw
would wait for a whole build instead, so the low queue does not rest then. The stage library prebuild (`lb_worker()`)
does not rest: its list starts with the compute shaders the game is about to dispatch, a compute pipeline not built is
built on the replay thread at its dispatch, and the large compute builds (`bg_main()`, which hold a frame for seconds
when left to their dispatch) start only once it is done. Nothing rests during an area load, while the boot screen waits
for the warm (`bf_waiting()`) or in a preparation. `runtime/vk/vk_backend.c` `frame_end()` gives each frame's draws
(`vk_pipeline_frame_note()`); the start logs the setting, and `save_with()` what the gate held back.

`PG`:

- `PlatMutex lock`: the counts.
- `volatile int32_t draws_min`: the setting, read once; -1 before.
- `volatile uint32_t draws_last`: the draws of the frame last shown.
- `volatile uint64_t shown_ns`: when it was shown (`plat_now_ns`); 0: none yet.

### `pg_draws_min()`

The play gate's setting, read and logged once, at the start (`ensure_init()`).

### `play_rest()`

1 when background builds rest now (`runtime/vk/vk_play_gate.h` `play_gate_rest()`). The moment is a light one in a
preparation, while the boot screen waits for the warm (`bf_waiting()`) and during an area load.

### `low_held()`

The low queue's jobs no rest has counted yet, now counted (`Job.rested`). Caller holds `P.lock`. Walked once a rest,
when it begins (not at each look in it): a job queued during a rest is counted by the next rest that finds it queued.

### `play_rest_wait()`

The library warm's rest before its next build (`lw_worker()`): it holds one build back.

### `pg_log()`

The log's line of what the gate held back, when that changed since the last one (`save_with()`: each save in play and
the one at the end).

### `vk_pipeline_probe_buildgate()`

`--probe-buildgate` (`runtime/host.h`): the gate with this machine's commit and the settings in effect, without a device
and without a build. It prints what the settings and the system give, asks for sixteen ordinary builds and one large
one, and checks the answers against the same numbers: the builds admitted are those that fit with the earlier ones'
reservations, a refused one reserves nothing, and a builder's wait ends at once when its stop flag is set.
`tests/build_gate_check.py` runs it with `build_free_mb` at 0 (everything admitted) and at a value no machine has free
(everything refused). The answers it expects use the one commit sample the gate took, which stays for
`GATE_SAMPLE_MS`.

### `fast_retire()`

Fast-linked pipelines the optimised build replaces.

With the graphics pipeline library a draw first gets a pipeline linked from stage libraries (`p->fast`), and the
optimised build of the same state replaces it in `p->info.pipeline` when it is done. The fast link was kept until the
process ended, so every pipeline a run drew held its fast link's driver memory as well: one play session of 13,800
frames made 1,108 fast links and replaced 907 of them, while the video memory outside the allocator, where the driver
keeps compiled pipelines, grew by 491 MiB (RTX 5090), and a 68,400-frame session grew it from 1,672 to 2,485 MiB. A
replaced fast link is destroyed now, once the GPU is past every submission that may have recorded it. The draws read
`p->info.pipeline` when they record (`runtime/vk/vk_exec_draw.c`, the pipeline memo reads it again each time) and keep
no handle past the submission they record, so after the store in `job_build()` a draw can only have the old handle in the
submission being recorded, or in an earlier one: `vk_sync_issued()` + 1 at most. The destroy waits for one more
submission than that, for a recording thread that runs a submission behind (`runtime/vk/vk_rec.c`). It runs on the
completion thread (`vk_completion_add()` of `runtime/vk/vk_sync.c`), which holds no lock of ours while it calls it; a
lost device or the end of the process leaves it to the driver, as everything else.

Called with `P.lock` held and `p->info.pipeline` just replaced by the optimised pipeline. A full memory fence makes the
new handle visible before the submission count is read. `fast_retired` (under `P.lock`) counts the fast links
destroyed so far.

### `pressure_release()`, `release_drain()`, `releasable()`

Pipelines no draw has used, given back while video memory is short.

The boot builds the recorded pipelines ahead of play (the prewarm and the shipped pipeline list), and the driver keeps
each one's compiled code in video memory until it is destroyed. In a 4K session with DLSS and frame generation the
device-local memory outside the allocator was 4.2 GB with 6,300 graphics pipelines alive, 3,589 of them not drawn once
in that run. On a card short of video memory those are the cheapest thing to give back: no draw has recorded them. So
while the device-local memory in use is over `NX2SARIA_TEX_VRAM_PRESSURE` percent of the budget (`vk_mem_vram_live`,
which `NX2SARIA_VRAM_BUDGET_MB` caps for a test), the autosave thread takes up to `RELEASE_BATCH` graphics pipelines
that no draw has looked up in this run out of the table every `RELEASE_EVERY_MS`, and destroys them with their entries
once the GPU is past the submission being recorded and one more (as `fast_retire()`), at most `RELEASE_DRAIN` a tick
(`release_drain()`). Not on the completion thread: a value counts as done only after its callbacks ran
(`runtime/vk/vk_sync.c` `completion_main()`), so a batch of destroys there held every fence wait of the drawing threads.
A draw that needs one later misses it as it would a pipeline never built (`pipe_miss()`): its stage libraries link it,
or the look-ahead queued them (`vk_pipeline_hint()`), and its optimised build comes from the pipeline cache, which still
holds it. Only entries no lookup reached go (`releasable()`, under `P.lock`: `used_min` 0; `pipe_minute` is set before
the first lookup,
`ensure_init()`), so no draw's memo (`PM` in `runtime/vk/vk_exec_draw.c`) nor any caller holds their
`VkPipeInfo`. Not taken: mesh pipelines (without stage libraries their build is long), entries building or with a link
queued, failed ones, and compute pipelines. The next pass logs the device-local use then and now, so a log says what a
batch gave back.

The use counts the free space inside the allocator's blocks as free, as the texture evictions do
(`runtime/vk/vk_texture.c` `sweep_pressure()`): the driver's figure (`vk_mem_vram`) counts it as used, so with free
blocks it could stay over the line while pipelines went, and every pass would give back another batch until no unused
pipeline was left. `NX2SARIA_PIPE_RELEASE=0` keeps every pipeline for the run, as before 0.10.0, and so does
`NX2SARIA_TEX_VRAM_PRESSURE=0`, which turns the textures' giving way for video memory off
(`runtime/vk/vk_texture.c` `sweep_pressure()`), and the pipelines' with it.

- `RELEASE_DRAIN` (64): destroyed per autosave tick (500 ms) at most: faster than `RELEASE_BATCH` per
  `RELEASE_EVERY_MS`.
- `rel_used0`: device-local use (MiB) when the last batch went; 0 none to report.
- `rel_head`, `rel_tail`: entries out of the table, linked through `next` in the order they went (so by `rel_fence`),
  until `release_drain()` destroys them. Only the autosave thread touches the list (`pressure_release()`,
  `release_drain()`): no lock.
- `release_drain()` runs on the autosave thread, each tick, and destroys the released entries whose fence the GPU is
  past. None after a device loss (`runtime/vk/vk_device.c` `vk_device_lost_check()`): the device is not called any
  more.

### `job_build()`

The whole build of job `j`, which the caller took off its queue, and the job's end: its pipeline is entered, the
threads waiting for it are woken and `j` is freed. `lowrun`: 1 on a pool worker that took `j` from the low queue.
Caller holds `P.lock`, which is released for the build. A failed optimised build keeps the fast-linked pipeline.
`PERF_CNT_PIPE_ASYNC` counts a build a draw waited for.
### `pipe_worker()`

A pool worker's loop, under `P.lock` except while it builds:

1. It sleeps while there is nothing for it. With 3 or more workers one never takes low-queue work, so a waiting draw's
   library starts at once (not with `NX2SARIA_PIPE_WAIT=draw`).
2. A library a draw waits for (`U.head`) comes first (`u_run()`, at normal priority).
3. Then the draw queue; with that empty, a low job, which passes two gates. A heavy frame in play (`play_rest()`,
   only with stage libraries): the low job stays queued and the worker is at rest (`resting`, since `rest_t0`). A draw
   that needs its pipeline moves it to the draw queue (`promote()`), which wakes this wait; otherwise the frame is
   looked at again in 100 ms. A resting worker counts as idle: a draw that waits gives its job to the pool rather than
   building it on its own thread (`job_wanted()`). No room for a background build (`gate_enter()`, `held` being the
   job's reservation): the job stays queued the same way, and the gate is asked again in 2 s; the log says when the
   builds wait and when they go on.
4. The thread's priority follows the job's (`A.prio_low`), changed under `P.lock`, so a draw raising this thread
   (`promote()`) is not undone.
5. Step 1 for a job a draw waits for: its stage libraries and the fast link (`gpl_link()`, which releases `P.lock`).
   Step 2, the optimised build, goes to the head of the low queue, so a burst of new pipelines gets every fast link
   before any optimised build. A job without a fast link is built whole at once (`job_build()`).

### `workers_start()`

Starts the pool on first use: a quarter of the logical processors, 2 to 4. Caller holds `P.lock`.

- The pool's threads are kept in `A.thread`: `promote()` raises a worker running a prebuild job.
- Workers for stage libraries and fast links alone: two (the vertex and fragment libraries of one waiting draw in
  parallel), and as many as the pool has with the look-ahead, which queues the libraries of a whole submission's new
  pipelines at once (`vk_pipeline_hint()`), or with the fast links of skipped draws (`lib_worker()`). They sleep unless
  a library or a fast link is queued.

### `workers_drain()`

Before the shutdown: queued prebuild jobs are dropped, not built, and queued library jobs too, whose entries then read
as failed builds (a draw's fast link, `link_queue()`, names its pipeline, not a library). Then it waits for what is
running.

### `promote()`

A draw needs a pipeline a prebuild job owns: the job moves to the draw queue, or its worker to normal priority. Caller
holds `P.lock`. A job moved to the draw queue also wakes a library worker for its fast link (`lib_worker()`).

### `job_wanted()`

A draw waits up to `wait_ms` for the pipeline of job `j`, which has no fast link (a pipeline without stage libraries, as a
mesh pipeline is with `NX2SARIA_GPL_MESH=0`, or one whose link failed) and so needs its whole build. While the job is
still queued the wait buys nothing: the draw queue is first in, first out, and the jobs before it are the fast links and
builds of other draws' misses, so on a burst of new pipelines the one a draw stood waiting for started last and the draw
was skipped at the end of its wait all the same.

- The job goes to the head of the draw queue and a worker is woken, so the next free worker takes it.
- A wait longer than `TAKE_WAIT_MS` is not one of a frame's budgets (`runtime/vk/vk_exec_draw.c` `frame_wait()` is at
  most 500 ms): the caller wants this pipeline whatever it takes (a one-shot draw with `NX2SARIA_ONESHOT_BUDGET=0`).
  With no worker idle the job is then built here at once (`job_build()`), as a library no worker took is (`lib_wait()`).
  A wait inside a budget is never turned into a build on this thread: a whole mesh pipeline takes 0.2 to 1.1 s to
  compile, and the budget exists so that the replay thread is not held that long.

Caller holds `P.lock`; it is released while the job is built here. A job that is running already, in the low queue or
gone is left alone.

### `mesh_pair_layout()`

The stage-independent layout of a mesh pipeline that has stage libraries (`mesh_libs()`): the mesh stage's bindings and
the task stage's after them in set 0, which the pre-rasterization library of the two is made with, and the fragment
stage's in set 1, which is the set of that fragment shader in a vertex pipeline too, so the fragment shader library can
be the same one. NULL with `*why` set. On success `*B` holds both lists, set 0 first (`layout_pair()`). Caller holds
`P.lock`.

### `pipe_miss()`

Enters a missing pipeline: built here (mode 0), or queued for a draw (1) or for the prebuild (2). Failures are entered
too. `*queued`: 1 when a job owns it. Caller holds `P.lock`.

- Mesh draws (`VK_EXT_mesh_shader`). With stage libraries (`mesh_libs()`) the layout is a stage-independent pair like a
  vertex pipeline's (`mesh_pair_layout()`). Without them, and for an emulated mesh pipeline, one set holds the mesh
  stage's bindings, the fragment stage's after them and the task stage's after those (`create_graphics()` renumbers both
  modules), and the pipeline has no fast link. They go to the workers like the others: built here, each new one stopped
  the frame for its whole compile (0.26-0.51 s in Kokiri Forest), and they are recorded for the boot prewarm
  (`keys_note()`).
- With a stage-independent layout the fragment module keeps its bindings, in set 1 (`span` 1, so `fs_off` 0).
- A job queued for a draw also wakes a library worker for its fast link (`lib_worker()`).

### `job_lower()`

A job still in the draw queue goes to the head of the low queue: its pipeline has (or is about to have) a fast-linked
one, so the optimised build waits behind other draws' misses. Caller holds `P.lock`.

### `lib_queue()`

Queues `s`'s missing pre-rasterization and fragment shader libraries at the tail of `U` (`u_push()`); `b`: both
entries. 0 when one cannot be entered (no layout half, which `gpl_link()` reports, or out of memory). Caller holds
`P.lock`.

### `link_queue()`

A waiting draw's whole fast link of `p` on the library workers: its library jobs, then the link job behind them. The
caller then waits for `p->info.pipeline`. 0 when nothing was queued (the caller links as before). Under `P.lock`.

### `lib_wait()`

A draw waits for pipeline `s`: its missing pre-rasterization and fragment shader libraries are queued for the workers
(`lib_queue()`), so they build in parallel on idle workers, and this thread sleeps until both are published or `end`
(`now_ms()`) passes. Without library workers, a job no worker took within 1 ms (or with none idle) is built here
(`lib_steal()`, which releases `P.lock` while building). Caller holds `P.lock`.

### `vk_pipeline_hint()`

Stage libraries ahead of their draws.

`NX2SARIA_PIPE_AHEAD`, default 1; 0 off; `runtime/vk/vk_exec.c` "looking ahead".

The replay names the state of a draw it has not reached yet. When no pipeline is known for it, its pre-rasterization and
fragment shader libraries are queued to the library workers now, so they build while the draws before it are recorded,
in parallel with each other and with those of the other draws named: the draw's own lookup then finds both and links on
its thread in microseconds instead of waiting for them or being skipped while they build. Nothing else is entered, no
pipeline, no optimised build and no key record: a state the scan guessed wrong costs two libraries at most. The feedback
loop bits, the topology and the targets do not matter here, as no shader library depends on them. A mesh draw is named
too where mesh pipelines have stage libraries (`mesh_libs()`): its task and mesh library is the one that takes longest
to make. Returns the libraries queued. `U.waits` and `U.queued` are put back after the queueing: those count waiting
draws' jobs.

### `graphics()`

The lookup of a draw's pipeline (`vk_pipeline_graphics()`, `vk_pipeline_graphics_async()`), under `P.lock`: found, or
entered by `pipe_miss()`. For a pipeline that is building and has no fast link yet:

- A draw that would wait (`wait_ms`) gets its missing shader libraries built at once, the vertex and fragment ones in
  parallel on idle workers (about 0.12 ms per library from the driver's disk cache, 5-180 ms cold): its own job may
  queue behind optimised builds of earlier pipelines. The job then only does the optimised build, from the low queue
  (`job_lower()`): its worker would only wait for the same libraries. Other draws leave the libraries to the job and
  are skipped meanwhile. `NX2SARIA_PIPE_WAIT=draw` builds them on this thread one after the other instead, as before;
  `NX2SARIA_GPL_DRAW_BUILD=0` leaves them to the job.
- With library workers the libraries and the link go to them (`link_queue()`), and this thread waits without building.
  `fast_tried` is set, so the job, if a worker takes it meanwhile, goes straight to the optimised build.
- Without them the libraries are waited for (`lib_wait()`, which releases `P.lock`) and linked here when both exist
  (`gpl_link()` with `build` 0); with `NX2SARIA_PIPE_WAIT=draw`, `gpl_link()` builds them (`build` 1 releases
  `P.lock`). A link a worker made obsolete by publishing a pipeline while the lock was free is destroyed.
- The wait that follows counts as the render thread's pipeline time. `job_wanted()` releases `P.lock` when it builds
  the job here.

### `warm_builder()`

Builds queued prebuild jobs until the low queue is empty or `K.until` passes, beside the `pipe_worker()` pool (same
bookkeeping). Jobs left queued at `K.until` stay for the pool. So do those the memory gate refuses (`gate_enter()`): a
builder it refuses ends, which leaves fewer builders compiling side by side while the others' reservations stand, and
none when the machine has no room; the pool builds what is left during play, when the gate lets it. arg non-NULL: one
job at most (`arg` non-NULL). The builders start with one pipeline built alone on the starting thread: a driver's first
graphics pipeline
sets up its compiler, and NVIDIA's 621.14 was reported to fault in a sizable share of starts when every builder began
its first pipeline at the same moment.

A job it takes is no longer low: it is not on a pool thread, so `promote()` has nothing to raise. A failed optimised
build keeps the fast-linked pipeline, as in `job_build()`.

### `vk_pipeline_prewarm_progress()`, `vk_pipeline_boot_stats()`

`pw_phase`: `vk_pipeline_prewarm_progress()`'s numbers: the phase, and the pipelines finished (built or failed) of those
queued, copied while `prewarm_run()` waits for its builders; in phases 3 and 4 (`bf_wait()`) the compute pipelines and
the prepared variants.

`pw_boot_built`, `pw_boot_ms`: the boot prewarm's pipelines built and its pipeline stage's time, once it ended
(`vk_pipeline_boot_stats()`); -1 before.

### `bf_waiting()`, `vk_pipeline_prewarm_detach()`

1 while the start waits for the builders of the whole warm: they run at normal priority on every core then, and as in
play (idle priority, the background's thread counts) from the moment the game starts.

`BF`: the whole warm on the boot screen after a driver change (`bf_decide()`).

- `int on`: this start waits for all of it.
- `int lw`: `lb_start()` left the library warm's start to `bf_wait()`.
- `volatile int32_t game`: the wait is over: the warm ended, or the boot let the game start beside it
  (`vk_pipeline_prewarm_detach()`).
- `double t_end`: `now_ms()` at which the whole warm's time on the boot screen is up; 0 no limit.
- `char owed[PLAT_PATH_MAX * 2 + 48]`: `boot-warm-owed.txt`.

### `vk_pipeline_prewarm()`

`PREWARM_PIPE_MS` (20 s) is the boot's share of the pipeline prebuild with the graphics pipeline library. The boot
builds the recorded pipelines in record order, so those met first in play (the title screen, the first places) come
first, for at most this long after the shaders are ready; the rest stay queued for the pool workers, which build them
below normal priority while the game runs. A draw that meets one of them before it is built gets a pipeline linked from
stage libraries at once (`gpl_link()`), as for any new pipeline in play. After a driver update, when every pipeline is
built from nothing, the boot waits for all of them and for the rest of the warm (`bf_decide()`). Without the library a
draw would have to wait for a whole build, so the boot waits for all; so it does with `NX2SARIA_PREWARM_FULL=1`.

When it ends, the session header is written again with the prewarm's numbers (`runtime/session_summary.c`
`session_summary_header()`): a crash after this point gets them in its summary.

### `mesh_pre_note()`, `mesh_pre_worker()`, `mesh_pre_start()`, `mesh_pre_run()`

Mesh pipelines' stage libraries ahead of their draws.

The shader preparation and the boot build the stage libraries of single shaders (`lb_library()`): one vertex or one
fragment shader is all such a library holds. A mesh pipeline's pre-rasterization library holds its mesh stage and its
task stage together, and its fragment shader library depends on what that mesh stage leaves unwritten (`FsKey`), so these
are taken from the pipeline records, which name all three shaders: every recorded mesh pipeline the boot queues
(`prewarm_run()`) and every listed one the preparation builds (`seed_pipelines()`) notes its two shader libraries here,
each key once. They are built once and destroyed, as the single shaders' libraries are, which leaves them in the
pipeline caches and the driver's own: a mesh pipeline of the same stages that a draw meets later (other targets, another
blend state or sample count, or a recorded one the boot has not come to) then finds its libraries there and is linked in
milliseconds, where it was compiled whole before.

- Preparation (`mesh_pre_run()`, from `seed_pipelines()`): on every core, before the pipeline cache is saved, so that
  file holds them too; they are as many as the list has mesh stage pairs, not the tens of thousands of single shaders'
  libraries that are kept out of it.
- Boot (`mesh_pre_start()`, from `lb_start()`): one thread beside the game at idle priority, behind the memory gate
  (`gate_wait()`); `NX2SARIA_LIB_PREBUILD=0` leaves it out with the rest of that prebuild.

`MeshPre`:

- `int kind`: `LIB_PR` or `LIB_FS`.
- `uint64_t h`: `key_hash()` of the key.

`MP`:

- `uint32_t n, cap, n_pr`: `n_pr`: the pre-rasterization libraries among them.
- `int boot`: the builder runs beside the game: idle priority, the memory gate.
- `PlatThread *th`: the boot's builder.
- `uint64_t built, have, fail`: `have`: a draw had made the library by the time its turn came.

`mesh_pre_note()` notes the two shader libraries of mesh pipeline `s` (key `k`), a pipeline with stage libraries
(`mesh_libs()`), for `mesh_pre_worker()`. Caller holds `P.lock`; no builder runs yet.

`mesh_pre_worker()`:

- It runs as the boot's other builders: at idle priority beside the game, at normal priority while the start waits
  (`bf_waiting()`). `P.lock` is never held at idle priority, as the replay takes it: the thread goes to normal priority
  before it locks.
- Each library built adds to `cache_merges`, for `autosave_once()`: no pipeline count tells of it.

### `seed_boot()`

`NX2SARIA_PIPE_SEED_BOOT` (auto, the default; 1; 0): whether the boot builds the pipelines of the shared list that this
install has never drawn (`keys_open_seed()`), 1, or leaves them to their first draw, 0. Built at boot they are there
when a draw first needs them, which is what the list is for, and every one is a pipeline the driver holds for the whole
run: with the list of 0.9.1 the boot's pipeline stage went from about 1.6 GB of commit (0.8.0, 3,256 pipelines on an RTX
5080) to 3.9-4.4 GB (8,531 on an RTX 2080 Ti, 6,977 on an RTX 5070 Ti) and 0.3-0.6 GiB of video memory, 1.5-1.8 GB of
commit on AMD's driver, of which the list's share is half to nearly all. On a machine that has that to spare it buys the
first visit of every area without a wait. On one that starts short of the commit the game takes in play
(`runtime/mem_need.h`, the start's own warning) it is memory taken from the game for pipelines that may never be drawn,
and running out of commit ends the game (a failed allocation or a lost device). auto leaves them out there and only
there. A pipeline left out is made at its first draw as any new one is, from `pipeline-cache.bin` when the preparation
built it (0.5-0.8 ms on a worker), and is the install's own from then on: the next start builds it at boot. Mesh
pipelines are built at boot either way. Without stage libraries a draw waits for their whole build. With them
(`mesh_libs()`) the first draw of one would be linked from its libraries, but those are the largest modules the game
has, and the few frames they take to come out of the caches are frames in which a kind of foliage is missing from a
place the player has just entered: built here, it is there from the first frame. The list's pairs still count in the
vertex output unions, so the modules stay the ones the preparation built.

### `prewarm_run()`

The boot's build of the recorded pipelines. With `NX2SARIA_PIPE_PREBUILD=0` it only starts the stage library prebuild
(`lb_start()`); `prebuild_start()` logs the off case.

1. The shaders the records name are translated or loaded (`gfx_shader_prewarm()`), with `NX2SARIA_PREWARM_XLATE`
   translator processes, half the CPUs and at most 16 when unset. `carried` and `fresh` count them by where their
   translation came from. After an update with another translator a record still finds its shaders, as it names them
   by input id, which no translator changes, and the pipeline cache gives the pipeline back wherever the translation in
   use is the one it was built from; the log line after the build says how many shaders changed.
2. With stage-independent layouts (`P.indep`) the vertex output unions are made first (`vs_outs()`): workers may take a
   job before the queue is complete.
3. When an earlier start on this GPU and driver died in these builds (`pm_skipped()`) nothing is built ahead of play.
   Otherwise the crash marker is written (`pm_set()`) and the whole warm decided (`bf_decide()`).
4. Each record is entered (`pipe_miss()`, mode 2). `pr_hist` and `fs_hist` count the library state of the vertex
   pipelines, which decides the single shaders' libraries (`lb_state()`, `lb_start()`); a mesh pipeline's libraries are
   built with its own state (`mesh_pre_note()`, `mesh_pre_start()`). A pipeline of the shared list alone, not a mesh
   one, is left to its first draw where `seed_boot()` says so.
5. Builders start (`warm_builder()`), the first pipeline alone, then one per CPU up to 64. With the library
   (`P.gpl`) the boot's share ends at `K.until = now_ms() + PREWARM_PIPE_MS`, set before the builders start; without
   it, with `NX2SARIA_PREWARM_FULL=1` or with the whole warm, the boot waits for all; with the library the whole warm
   keeps its own limit (`BF.t_end`), after which what is left goes to the pool's workers as after `PREWARM_PIPE_MS`.
   While it waits it copies the count for the boot screen every 100 ms and logs it every 2 s.
6. With nothing left in the low queue it waits for the jobs a pool worker took; otherwise the pool builds the rest while
   the game runs. Then the marker is cleared (`vk_pipeline_prewarm_marker_clear()`): what the pool builds later runs
   during play. The stage library prebuild starts (`lb_start()`), and the whole warm's wait follows (`bf_wait()`).

Mesh pipelines are dropped on a GPU that draws them through the mesh fallback (no mesh shaders and no mesh emulation):
none to build, as the preparation skips them too (`seed_pipelines()`). With the mesh emulation
(`NX2SARIA_MESH_EMULATION`) one is a vertex pipeline and its compute stages (`pipe_miss()`: `layout_get_emu()`), built
here like the others: left to the first draw, each took 113-830 ms on an RX 580 and the draws waiting for it were
skipped (22 of the 48 pipelines missed entering Kokiri Forest, 0.9.1), frames held back or shown without them. With the
mesh fallback on a device that has mesh shaders (`runtime/vk/vk_mesh_path.h`: AMD's driver for RDNA 1 and 2) none is
built either: no mesh draw uses one, and that driver is not to compile them.

A cache file of another device or driver (a driver update: `cache_data_read()`) leaves every recorded pipeline to be
built from nothing, and the ones the boot left after `PREWARM_PIPE_MS` were built beside the game: on an 8-core
integrated GPU that start had 2,141 of 8,122 left after 20 s and took 296 s to finish them. The whole warm
(`bf_decide()`) waits for all of them, with its count on the boot screen, as a start without the library does.

### Boot prebuild crash marker

The boot builds the recorded pipelines (`warm_builder()`) before the game starts. A driver whose compiler crashes on one
of them would kill every start at the same point, and the preparation's journal (Preparation that outlives a driver
crash, below) does not cover these builds. So
`<dir>`/`pipeline-prewarm-running.txt` exists while they run: written before the first one, deleted when the boot's
share ends (or when the boot stops waiting and lets the game start). A start that finds it left over records this GPU
and driver in `<dir>`/`pipeline-prewarm-skip.txt`, and starts on them build no recorded pipelines ahead of play: each
one is built at its first draw instead, as with `NX2SARIA_PIPE_PREBUILD=0`. Another driver version, or deleting the
file, brings the prebuild back. Only the boot's own share is covered: a crash during play, where the pool also builds
what the boot left, says nothing about these builds. An orderly end of the process during the boot (the window closed,
the guest's ExitProcess, a `--seconds` run) deletes the marker as well.

A marker left over is not yet a driver crash: a start stopped from outside (the task manager, a program that ends the
game, the power) leaves one too, and since the whole warm (`bf_decide()`) the boot builds for minutes. So the crash
handler says which it was (`vk_pipeline_crash_note()`: `<dir>`/`boot-build-crash.txt`, written while the boot's builds
run): with that file the next start skips the prebuild as before; without it the end is taken for a stopped start and
only counted, in the marker itself. `BOOT_QUIET_ENDS` such ends in a row skip the prebuild after all: a driver that
takes the process down without a fault our handler sees would otherwise end every start (`runtime/vk/boot_warm.h`
`boot_end_decide()`).

- `static volatile int32_t jr_boot_on`: the boot's journal of library builds is open (`journal_open()`).
- `static int pm_quiet`: starts in a row before this one that ended in the boot's builds without a crash report.

### `vk_pipeline_crash_note()`

`runtime/main.c` `crash_handler()`, before its report: a fault while the boot's pipeline builds or its journalled
library builds run. The file is all the next start needs; nothing here allocates.

### `crash_noted()`

1 when the last start left the crash handler's note; asked once a start, and the note goes.

### `pm_skipped()`

1 when this start builds no recorded pipeline ahead of play: `pipeline-prewarm-skip.txt` lists this GPU and driver. A
marker left over is settled first (`boot_end_decide()`): one of another GPU's or driver's builds is deleted; a crash, or
`BOOT_QUIET_ENDS` quiet ends in a row, adds this GPU and driver to the skip file; a quiet end is counted (`pm_quiet`).

### `pm_set()`

Writes the marker, the GPU and driver and the quiet ends so far, straight to the disk: the file has to be there if the driver takes the process down a moment later.

### The whole warm on the boot screen after a driver change

A start that finds `pipeline-cache.bin` written by another device or driver (`cache_data_read()`: a driver update,
another GPU) has nothing built: not the recorded pipelines, not the compute pipelines, and the driver's own cache of
stage libraries is empty as well. Left to the background as on any other start, all of it was compiled beside the game:
an 8-core integrated GPU drew fewer than 600 frames in its first 11 minutes, with the recorded pipelines done after 296
s and the compute pipelines after 570 s, the game's own compute dispatches compiling on the replay thread until then.
Such a start waits for all of it on the boot screen instead, with its counts (`runtime/boot.c` `boot_wait()`): the
recorded pipelines (`prewarm_run()`, phase 2), then the compute pipelines on up to `LB_MAX_THREADS` threads at normal
priority (`lb_start()`, phase 3), then, when a sample says the driver's cache does not hold the prepared stage libraries
(`lw_check()`), every prepared variant on up to `LW_BOOT_THREADS` threads (`lw_worker()`, phase 4). The memory gate
(`gate_wait()`) holds for all of them. The large compute shaders (`bg_main()`) are not waited for: their one thread runs
beside the library phase.

It takes minutes (with nothing in the driver's cache, an RTX 5090 and 16 CPU threads: 87 s for 5,775 recorded pipelines,
19 s for 4,075 compute pipelines, 317 s for 42,231 prepared variants), once per driver: `<dir>`/`boot-warm-owed.txt`
names the GPU and driver and counts the starts that began the warm (`runtime/vk/boot_warm.h`). A start that finds its
own GPU and driver there with a count goes on with a warm that an earlier start did not end; what was built comes back
from the caches in a moment. The start whose warm ends writes "done" there, which keeps the next starts ordinary ones:
`pipeline-cache.bin` keeps the earlier driver's header until the play cache is folded into it (`cache_load()`), some
starts later. After `BOOT_WARM_TRIES` starts that did not end the warm the file stays as well, and starts on this GPU
and driver are ordinary ones again. While the boot waits, the library builds write the preparation's journal
(`journal_mark()`), so a variant the driver's compiler dies on is skipped from the next start on (`skip_init()`) instead
of ending every start; a start that was only closed or stopped then blames no variant (the crash handler's note decides,
as at `pm_skipped()`).

The wait has a limit, `NX2SARIA_PREWARM_FULL_S` seconds from here (default 600, 0 none): the 425 s above are a fast
machine's, and a slow one would sit on the screen for half an hour. At the limit the game starts and what is left goes
on as on any other start: the recorded pipelines on the pool's workers, the compute pipelines and the library warm on
their idle threads (`vk_pipeline_prewarm_detach()`). The file then says "done" as well: the boot screen had its turn,
and the rest is the background's, as it always was. `NX2SARIA_PREWARM_WAIT_MS` and a minute without progress end the
wait the same way, as for any prewarm (`boot_wait()`).

`NX2SARIA_PREWARM_FULL=0` turns the whole warm off. Not for a `--shot` image, which no build timing may change and which
nobody watches.

### `bf_decide()`

Decides from `boot-warm-owed.txt` whether this start waits for the whole warm (`boot_warm_decide()`), and sets `BF`. A
file of another GPU or driver is deleted. Not with `NX2SARIA_PREWARM_FULL=0` or for a `--shot` image.

### `prebuild_start()`

`ensure_init()`'s hook: without a prewarm (`NX2SARIA_PIPE_PREBUILD=0`, or the device came up first elsewhere) the
records are not built. Caller holds no lock.

### `spv_override()`

`NX2SARIA_SPV_OVERRIDE`=`<dir>` (test): a compute shader whose SPIR-V hashes (FNV-1a over its bytes, the name
`runtime/vk/vk_dump.c` gives it) to a file `<dir>`/`<hash>`.spv runs that SPIR-V instead, an edited copy with the same
interface, to see a shader's intermediate values in a frame dump. NULL: no override; otherwise malloc'd words, their
count in `*out`.

### `fog_skip_block()`

1 when the block at word `at` (an OpLabel) is a skipped froxel's exit in the fog's first compute pass: five image writes
(OpImageWrite, 99), four of (0,0,0,1) and last the mask value (1/255,0,0,0), each an OpCompositeConstruct (80) of four
one-word OpConstants (43), then OpReturn (253). An OpLabel, OpBranch or OpBranchConditional (248, 249, 250) before the
return means another block.

### `spirv_fog_keep_froxels()`

The volumetric fog's first compute pass skips a froxel behind a coarse depth table and writes it as empty, and the next
pass's mask keeps it empty. That table comes from the frame before, so in the columns a near object has just left, every
froxel past the object's old depth came out with no fog: a dark block of the fog grid beside the object, which the fog's
history then smeared into a trail, and worst at the screen edges, where near objects move fastest. A froxel behind
today's depth is never seen, so computing it only costs a little time: the comparison is made never true (x > x).
Returns the patched copy of that shader, NULL for any other. Found by its shape rather than a hash, so it holds across
translator updates: a branch whose other side is that exit, taken on !(distance > table) || flag. `NX2SARIA_FOG_CULL=1`
keeps the game's skip.

`def` holds the word of each id's definition, for the OpLabels and the instructions the match looks at. The match: an
OpBranchConditional (250) with an OpSelectionMerge (247) just before, whose false side is that exit
(`fog_skip_block()`), on an OpLogicalOr (166) one of whose operands is an OpLogicalNot (168) of an OpFOrdGreaterThan
(186). Exactly one such comparison is patched: its second operand is made its first.

### `compute_warm_remember()`

No in-flight entries: concurrent identical requests may both build, but a failed request never suppresses a retry.

`ComputeWarmKey`: Successful cache-only warm requests. Input ids can differ while their final SPIR-V and pipeline
layouts match. Only preparation that destroys its pipeline may reuse these entries; a dispatch still needs its own live
handle. The code is hashed after overrides and fog changes. Subgroup requirements follow that code and the fixed device
capabilities. Layout handles and creation flags distinguish otherwise identical shader modules.

### `compute_create()`

A compute pipeline for cs in layout l; `VK_NULL_HANDLE` on failure. No lock needed (`P.cache` is driver-synchronised).
`cached_only` (needs `P.cache_control`): from `P.cache` or the driver's own disk cache, never compiled; `VK_NULL_HANDLE`
when neither holds it (the driver returns `VK_PIPELINE_COMPILE_REQUIRED`, a success code, with no pipeline).
`already_warm` is only supplied by callers that need cache coverage, not a live pipeline: 1 and `VK_NULL_HANDLE` mean an
identical request already succeeded in this process. NULL preserves normal creation and has no hashing cost.
`NX2SARIA_COMPUTE_WARM_REUSE=0` disables this reuse for comparisons without changing the prepared inventory.

- Pinned until the pipeline is made: a dispatch on the drawing thread that creates its pipeline here decodes a
  compressed shader first, about 60 us for a mean module against the creation's milliseconds (`runtime/gfx/gfx_shader.c`
  "SPIR-V at rest", counted in the memory report).
- Subgroup operations in a workgroup whose width is a whole number of subgroups need every subgroup full: the driver may
  otherwise run partial ones, and a ballot over the culling's candidates then counted differently from one frame to the
  next.
- `if (compute_warm_remember(&warm_key, 0))`: before the module: a reused request makes none.

### `compute_layout()`

The layout of a compute shader in `*B`; NULL with `*why` on failure. Caller holds `P.lock`.

### Stage library prebuild

Two places build what a draw would otherwise build while the game waits:

- `nx2saria.exe` `--prepare-shaders` (`lb_warm_prepared()`), after the translation, on every core: for each prepared
  variant (`shader-prepared.bin`) the pre-rasterization library of a vertex shader, the fragment-shader library of a
  fragment shader and the pipeline of a compute shader, each built from a private shader copy and destroyed at once.
  What stays is the driver's own disk cache: on AMD a library built again in a later process takes 0.1 ms instead of 55
  ms (vertex) / 28 ms (fragment). Our `pipeline-cache.bin` is not written there: it would grow by about 10 KB per
  library (~240 MB) and be read into memory at every boot.
- Boot (`lb_worker()`), after the pipeline-key prewarm, on two threads (`lb_start()`) at `PLAT_PRIO_IDLE` (the short
  `P.lock` sections and shader loads at normal priority, so a preempted thread never holds the lock the replay needs; a
  compute pipeline's compile below normal priority, `lb_compute()`): compute pipelines of every recorded and prepared
  compute shader (`vk_pipeline_compute()` builds compute synchronously on the replay thread; 163 prepared): those of the
  shaders dispatched most recently stay, the others are built into the pipeline cache and destroyed (see
  `compute-used.bin`). Then the libraries of the first `NX2SARIA_LIB_PREBUILD_KEEP` variants play recorded (priority 0),
  kept in `libs[]` with their shaders. KEEP defaults to 0: a kept library costs 140-460 KB of process memory (10,000
  kept took 9 GB in run 1) to save a rebuild the driver cache makes 0.1 ms.

Library state: the vertex output union known now (`vs_union_peek()`; the prewarm fills it from `pipeline-keys.bin`, the
preparation has none), the clamp, polygon mode and depth mode most recorded pipelines use (defaults without a key file:
clamp 0, fill, -w..w), the sample count most recorded pipelines use (single-sampled without a key file), no dual
source (`lb_state()`); the state dynamic on this GPU is 0 in every key. A draw that
meets such a variant in a new pipeline then gets its libraries from `libs[]` or the driver cache and fast-links
(`gpl_link()`). `NX2SARIA_LIB_PREBUILD=0` turns the boot part off, =N stops it after N variants;
`NX2SARIA_LIB_PREBUILD_THREADS` sets its thread count, `NX2SARIA_LIB_PREBUILD_BIG` its compute size limit
(`LB_BIG_WORDS`).

The boot part builds a compute shader over `LB_BIG_WORDS` SPIR-V words only from the pipeline caches (`compute_create()`
`cached_only`); one that no cache holds goes to the background builder (Large compute shaders on a background thread,
below), or with `NX2SARIA_LIB_PREBUILD_BIG_BG=0` is left to its first dispatch, like any shader outside the warm list. Play
recorded four straight-line compute shaders of 0.87-4.4 million words (one function of up to 888K instructions; the
median of 3,648 compute shaders is 7,351 words, p99 64,844). NVIDIA 621.01 on an RTX 4090 compiled them on four prebuild
threads for minutes at 100% CPU each while the process grew from 12 GB to 28-31 GB, until RAM ran out and the game
stuttered and went black a few minutes after boot. None finished, so no cache held one and every boot did it again.
`NX2SARIA_LIB_PREBUILD_BIG` sets the limit in words; 0 builds every size.

`LB`:

- `size_t n_keep`: e[0..`n_keep`): compute shaders whose pipeline stays (`cu_keep()`).
- `volatile int32_t short_mem`: the memory gate refused a build for `GATE_WAIT_MS`: the workers ended.
- `uint8_t clamp, poly, minus_w, samples`: library state (GKey fields).
- `double t0, ms[3]`: ms: build time per kind (vertex, fragment, compute).
- `uint64_t built[3], have[3], fail[3], warmed[3], noshader, nolib, skipped, done, cs_draw`: `warmed`: built for the
  caches only and destroyed (`lb_count()`); `nolib`: vertex and fragment variants skipped with the graphics pipeline
  library off; `skipped`: variants of the skip list (`SK`, below).
- `uint64_t big_words, deferred`: the size limit (`lb_big_words_init()`); compute shaders over it left to their dispatch.
- `uint64_t big_unused`: compute shaders over it that no run dispatched: not built at all.
- `volatile int32_t kept`: libraries entered in `libs[]` so far.
- `int idle`: build at `PLAT_PRIO_IDLE` (boot), normal (preparation).
- `int tail_one`: automatic boot policy: only worker 0 takes the warming tail.
- `int bg_threads`: boot: the workers that go on beside the game (`lb_next()`).
- `static __thread int lb_me = -1`: on a boot prebuild thread (`lb_worker()`): its index in `LB.th`.

### `lb_idle()`

The boot's builders run beside the game, at idle priority; not while the start waits for them (`bf_waiting()`).

### `lb_next()`

Takes the next boot job (a compare-and-swap on `LB.next`); `LB.n` when there is none for this worker. Beside the game
(`lb_idle()`) the workers from `LB.bg_threads` on take none: the whole warm (`bf_wait()`) starts more workers than go on
beside a game, and those end here once the game runs. With `LB.tail_one` (no `NX2SARIA_LIB_PREBUILD_THREADS`) only
worker 0 takes the jobs past `n_keep`: the compute shaders dispatched most recently are built in parallel, and one worker
goes on with the rest of the list. The compare-and-swap makes that boundary exact when two workers reach it together; no
pipeline creation or dispatch waits on it. The preparation uses its own queue at full concurrency.

### Preparation that outlives a driver crash

A bug in the driver's shader compiler can kill the process inside `vkCreate*Pipelines`: NVIDIA 615.78.08 on an RTX 5080 crashed on the same vertex library at the same point of every
preparation, natively and under Wine, so preparation never finished (issue #30 and a report on PR #60). While
`lb_warm_prepared()` builds, slot k of `<dir>`/`shader-prepare-journal.bin` holds the variant its worker k has handed to
the driver (input id and stage; zeros between variants). The writes go straight to the OS, so they outlast the process,
and a run that finishes deletes the file. The next preparation or boot (`skip_init()`) moves the variants a leftover
journal names into `<dir>`/`shader-prepare-skip.txt` with this GPU and driver version, and preparation and the boot
prebuild (`lb_worker()`) skip what that file lists for them. The variants the other workers were building at the crash
are skipped along with the culprit; like it, they build at their first draw, where the driver can still crash. Another
driver version builds them again, and so does deleting the file. `tools/prepare_shaders.py` runs the build again
(`--warm-shaders`) after a run that left a journal.

- `#define JR_SLOT 64`: bytes per journal slot: input id, stage, zeros.
- `#define JR_SLOTS 64`: `lb_warm_prepared()`'s thread cap.

`SK`:

- `PlatFile *journal`: while `lb_warm_prepared()` builds, and while the start waits for the library warm (`bf_wait()`).
- `PlatMutex jlock`: journal: `bf_wait()` and the process's end close it while workers write.
- `char jpath[PLAT_PATH_MAX * 2 + 48]`: the boot's journal, "" without one.
- `uint8_t (*iid)[32]`: the skip list's variants for this GPU and driver.

### `cache_dir()`

The folder of `pipeline-cache.bin` and `shader-prepared.bin`.

### `gpu_key()`

vendor:device:driver version, as the skip list names a GPU and driver.

### `journal_fold()`

The variants a leftover journal j (jn bytes, of file jp) names join the skip list's text `*s` (`*sn` bytes; NULL without
a list) for this GPU and driver, and the list's file sp is written again; the new text, NUL-terminated, replaces `*s`,
which `skip_init()` parses next. 1 when the journal may go: nothing in it, the list holds it now, or no memory for the
new text.

### `skip_init()`

Folds a journal that a run which died left into the skip list, then reads the skip list's variants for this GPU and driver into SK. The
preparation's journal (`shader-prepare-journal.bin`) always counts. The boot's (`shader-warm-journal.bin`,
`journal_open()`) counts only with the crash handler's note (`crash_noted()`): without it the start was closed or
stopped while it waited, and the variants it happened to be building are no one's fault.

### `journal_mark()`

Journal slot `slot`: the variant e its worker hands to the driver next, or zeros (e NULL) when the build returned.

### `journal_open()`

The boot's journal, for the library builds the start waits for (`bf_wait()`), in a file of its own: `skip_init()` treats
one left over differently from the preparation's. `skip_init()` has run (`lb_start()`) and has dealt with a leftover
one.

### `journal_close()`

Ends the boot's journal: when the library warm is done, when the game starts beside it (a crash in play says nothing
about the variant a background builder has in hand) and when the process ends in order during the wait
(`vk_pipeline_save_exit()`), so that only a process that died while the start waited leaves one. The builds still in
flight write nothing afterwards (`journal_mark()`).

### `cs_flight_has()`

`cs_flight`: compute shaders whose pipeline a thread is compiling now (`P.lock`): a cold driver takes about 2 s for one of
the game's large compute shaders. The replay used to compile under `P.lock`, which held every other pipeline lookup that long, and
compiled a second copy of a pipeline the boot prebuild (`lb_compute()`) was already compiling.

### `cs_flight_add()`

Enters cs in `cs_flight`. th: the thread a waiting dispatch raises to normal priority, NULL when it compiles at normal
priority already. 0 when the table is full (the caller compiles anyway).

- `static volatile uint8_t cs_flight_want[CS_FLIGHT]`: a dispatch waits for the slot's pipeline
  (`vk_pipeline_compute()`).
- `static PlatThread *cs_flight_th[CS_FLIGHT]`: the boot prebuild thread compiling it below normal priority.

### `cs_flight_wanted()`

A dispatch waits for cs's build: a builder that would only have filled the pipeline cache keeps the pipeline. Returns
the thread to raise, the first time.

### `cs_flight_is_wanted()`

Without `P.lock`, for the thread that holds cs's slot: whether a dispatch came to wait.

### `cs_flight_remove()`

1 when a dispatch waited for it.

### Compute pipelines the boot keeps

`compute-used.bin` next to `pipeline-keys.bin`; `NX2SARIA_COMPUTE_KEEP`.

The boot prebuild (`lb_compute()`) used to build the pipeline of every compute shader of the warm list (the ones play
recorded and the prepared ones, `shader-prepared.bin`) and keep all of them for the process: 4,066 on an installed copy
(42,344 variants in its list, 42,142 of them prepared), of which a boot into the first village dispatched 95. What that
cost was twenty of them. A copy's list has about twenty compute shaders of 0.9 to 2.0 million SPIR-V words: the game's
tensor-core upscaler programs, which the game never dispatches natively (`runtime/vk/vk_exec_draw.c` `native_tensor()`)
and which no run of the usage record below ever dispatched. A pipeline of one holds about 100 MiB of device memory from
the moment it exists, used or not, and the driver keeps that memory when the pipeline is destroyed. On an RTX 5090
(NVIDIA 621.01) a boot pass that read all 4,066 pipelines from `pipeline-cache.bin` took the device memory from 386 to
2,398 MiB although it destroyed 3,949 of them at once; the same pass before the twenty were in the cache left it at 386,
and building only the twenty took it to 1,370 (`--probe-prewarm`, 2026-11-10; the game showed 608 to 2,743 MiB over the
same stage). The 4,046 others are cheap: all of them kept cost 2 MiB of device memory and 0.7 GB of the process's
commit. So a copy with prepared shaders and a filled pipeline cache held 2 GiB for nothing from the title screen on, on
cards that need that memory for textures.

What the boot does now:

- A compute shader over `LB.big_words` is built ahead only when a run has dispatched it (`lb_compute()`, its keep
  argument). One that no run dispatched is not created at all: not from the pipeline caches and not by the background
  builder (`bg_main()`). If the game does dispatch one for the first time, that dispatch builds it, which holds the
  frame for the compile once (0.65 to 1.9 s measured for such a shader), and the record has it built ahead from the next
  boot on.
- Of the others the boot keeps the pipelines of the `NX2SARIA_COMPUTE_KEEP` (default 512) dispatched most recently, and
  builds every other one into the pipeline cache and destroys it at once (`lb_compute()` keep 0), from a copy of its
  shader that is released again (`lb_worker()`): the cache then holds it, so its first dispatch creates it without
  compiling. On that machine 94 first dispatches in play took 0.24 ms on average and 1.6 ms at most, 14 of them in the
  busiest 33 ms; reading the shader from the disk at that moment adds 0.05 to 0.15 ms (`--probe-prewarm`).

The record: one entry per compute shader a run dispatched (its input id) with the number of the last run that did; each
run that dispatches anything takes the next number. Kept at boot are the `keep_n` entries with the highest numbers, so a
short run does not push out what a long one used, and nothing is forgotten until newer shaders take its place. Without a
record (the first run that has one) nothing is kept and the run pays the first dispatches once.
`NX2SARIA_COMPUTE_KEEP=all` builds and keeps every compute shader of the warm list, the large ones too, as before; 0
keeps none.

`CU`:

- `uint8_t *keep`: per record read at boot: among the `keep_n` dispatched most recently.
- `uint32_t n, cap, n_file`: `n_file`: the records read at boot (keep has one byte for each).
- `uint32_t *slot`: open addressing by the id's first bytes: record index + 1, 0 empty.
- `uint32_t run`: this run's number.
- `int quiet`: `--probe-prewarm`: its dispatches are not the game's, the file stays as it is.
- `int32_t keep_n`: `NX2SARIA_COMPUTE_KEEP`; -1: all.
- `uint32_t kept`: records flagged in keep.

What the first dispatches of compute shaders cost this run (under `P.lock`): each creates its pipeline on the replay
thread. burst: the creations that fell into one window of `CF_WINDOW_MS`, the worst such window kept, which is what a
player would feel as one long frame when a new area starts.

`CF`:

- `uint64_t n, hit, pre_used`: hit: the pipeline cache held it (`cc_hit` 1); `pre_used`: kept from the boot and
  dispatched.

### `cu_slot()`

The id's slot, or the empty one it would take. Caller holds `CU.lock`.

### `cu_room()`

Room for one more record and a table at most half full. 0 when out of memory. Caller holds `CU.lock`.

### `cu_run_cmp()`

{run, index} pairs, the highest run first.

### `cu_parse()`

The number of records in a `compute-used.bin` image (f, size) with its header in `*h`, or -1 when it is not one.

### `cu_read()`

Reads the record at `CU.path` and marks what the boot keeps; called by `cu_open()` and `cu_open_prep()`.
`NX2SARIA_COMPUTE_KEEP` that is neither "all" nor a number reads as the default. An id in the file twice: the first
entry stands. A file of another format or version is rewritten.

### `cu_open()`

The boot's record: `compute-used.bin` next to `pipeline-keys.bin`. Caller holds `P.lock` (`ensure_init()`), after
`keys_open()`.

### `cu_open_prep()`

The preparation modes open no key file (their `ensure_init()` runs without `A.on`), so `cu_open()` leaves the record
closed and `cu_keep()` keeps every shader. They read the same record from the game folder dir, for `lb_big_left_out()`
to answer as at the boot, and never write it (quiet): they dispatch nothing. A folder without the record (a first
install) keeps none, as a boot without it does.

### `cu_keep()`

1 when the boot keeps the pipeline of the compute shader with this input id. With `NX2SARIA_COMPUTE_KEEP=all`, or
without the record (no cache folder), every one, as before.

### `cu_note()`

The first dispatch of cs in this run: its entry takes this run's number. The runtime's own shaders (no input id) are
built at their first use every run and are not recorded.

### `cu_write()`

Writes the record to path. 1 when written.

One writer at a time, from the copy of the record to the end of the file write: the autosave thread and a save at exit
can both be here, and the older copy must not land on the newer file.

The copy is allocated outside `CU.lock`, which a dispatch takes (`cu_note()`), with room for 256 entries that come
meanwhile. A write that fails leaves the record dirty: the next flush tries again.

### `cu_flush()`

Writes the record when a dispatch changed it (the autosave thread and the save at exit).

### `cf_note()`

A compute pipeline created at a dispatch took ms. Caller holds `P.lock`.

### `cf_log()`

The line of `CF`. `CF` is copied under `P.lock` held shared and the line written after the unlock: every dispatch needs
`P.lock`, and this runs on the autosave thread below normal priority.

### `vk_pipeline_compute()`

A dispatch's compute pipeline, found or built on this thread. Its key is the shader alone (`{cs, 1}`, without padding:
the key is hashed and compared as bytes).

- While another thread compiles it (`cs_flight_has()`), the dispatch waits for that result rather than compiling a
  second copy, and raises the builder: a boot prebuild thread compiling it below normal priority goes to normal and keeps
  the pipeline it would only have built for the cache (`cs_flight_wanted()`), and the background builder goes to normal
  (`BG.raised`). A wait of 2 ms or more is logged, 16 times at most.
- Otherwise it compiles here without `P.lock`, in a `cs_flight` slot, and counts the time (`cf_note()`). Another thread
  may have added it meanwhile (no flight slot was free): its pipeline stands, this one is destroyed.
- The first dispatch of this run notes the shader (`cu_note()`), so the boot of the next runs keeps its pipeline.

### `lb_count()`

what: 0 built, 1 already there, 2 failed, 3 cache only.

### `vs_union_peek()`

`vs_union_missing()` for vs alone, without entering vs into the union table (it holds `VS_OUTS_CAP` shaders).

### `lb_defer()`

A compute shader over `LB.big_words` that no pipeline cache holds: its first dispatch builds it
(`vk_pipeline_compute()`).

### Large compute shaders on a background thread

`NX2SARIA_LIB_PREBUILD_BIG_BG`, default 1; 0 leaves them to their first dispatch as before.

The boot prebuild leaves a compute shader over `LB.big_words` that no pipeline cache holds (`lb_compute()`), and every
one of them on a device without `pipelineCreationCacheControl`, where the caches cannot be asked. Its first dispatch then compiled it on the replay thread: frames of 0.65-1.9 s in Kokiri Forest on an
RTX 5090 after a translator change. One thread builds them instead, once the boot prebuild is done, one at a time and
the smallest first: four compiled side by side grew the process by 16-19 GB on an RTX 4090 (Stage library prebuild).
The shader being built is in `cs_flight`, so a dispatch that needs it waits for this build (`vk_pipeline_compute()`
raises the thread to normal priority then) instead of compiling a second copy. What is built is entered for the process,
as `lb_compute()`'s pipelines are, and lands in `pipeline-cache.bin` for the next boot.

`BG`:

- `const GfxShader *volatile building`: the shader on the driver now, NULL between builds.
- `volatile int32_t raised`: a dispatch waited for it and raised the thread.

### `bg_queue()`

A large compute shader no cache holds goes to the background builder (from the `lb_worker()` threads; under
`LB.stat`). Out of memory, it is left to its first dispatch (`lb_defer()`).

### `bg_room()`

`NX2SARIA_BIG_BUILD_FREE_MB` (default 8192; 0 no check): the system commit headroom a large compute build waits for. A
build of one of these shaders took the process up by 1-3 GB on an RTX 5090 and four at once by 16-19 GB on an RTX 4090;
started on a machine near its commit limit it can end the game. While the headroom is short the shader waits, and if the
game dispatches it first it is built there, as without the background builder. The builds under way elsewhere count
through their reservations at the memory gate (`gate_enter()`), where this build then reserves `GATE_BIG_MB` of its own
for as long as it runs, so the other background builds leave it its room.

### `bg_growth_cap()`

`NX2SARIA_BIG_BUILD_MAX_MB` (default 4096; 0 no cap): how far the builder may raise the process commit before it stops
and leaves the rest to their first dispatch. One of these shaders costs the driver 1-4 GB: on a 31 GB machine four of
them took the process from 15 to 29 GB and system commit to 60 of 65 GB, which is where a player feels the game stutter.
The builder exists to remove hitches, not to trade them for paging, so a machine that cannot hold them all builds only
what it can and pays one hitch per shader later.

### `bg_merge()`

The builder compiles into a pipeline cache of its own (`own_cache` at `pipe_create_in()`), not into `P.cache`. A create
into `P.cache` holds `cache_rw` shared from start to end, and the cache is only saved at a moment when nobody holds it:
on an RX 7900 XT (26.10.1) the largest of these shaders (4,874,502 SPIR-V words) was still compiling 590 s after the
boot, `pipeline-cache.bin` was not written once in that time, and the game was closed before the build ended, so the
next boot found neither that shader nor the pipelines built in play and started the same build again. `bg_merge()` moves
the result into `P.cache` when the build is done, at a moment with no build into `P.cache` running: the lock is tried as
`cache_save()` tries it, because a waiting exclusive request would hold back every new build. The merge goes into the
play cache when there is one, as what play compiled does, so its file gets it and not `pipeline-cache.bin`.

### `bg_main()`

The builder's loop, below normal priority. The queue can grow while this thread builds (`bg_queue()` runs on the replay
thread and reallocs these arrays), so the sort (smallest first: an insertion sort of a handful) and every read happen
under `LB.stat`; an entry is copied out before its compile so no pointer into the arrays is held across it. For each
shader:

1. The process's growth since the builder started is checked against `bg_growth_cap()`: past it the builder stops.
2. It waits for room (`bg_room()`), not under `P.lock`: a dispatch builds the shader meanwhile. `held` is this build's
   reservation at the memory gate, -1 none. After 30 s without room it stops: waiting is only useful when the headroom
   will come back. Stopped right after the gate let it in, it gives the reservation back.
3. A shader a dispatch got to first (in flight or entered) is counted and passed over. Otherwise it takes a `cs_flight`
   slot, through which `vk_pipeline_compute()` raises this thread (`BG.raised`), and builds into a pipeline cache of its
   own (`own_cache`; without one the build goes into `P.cache` as before).
4. The pipeline stays: only a shader a run dispatched is queued here (`lb_compute()`). It is entered before the merge
   (`bg_merge()`), so a dispatch that comes meanwhile finds it and does not wait for the merge. One a dispatch built
   meanwhile (no flight slot was free), or that cannot be entered for want of memory, is destroyed.

### `bg_start()`

The last boot prebuild worker starts the builder when it queued anything.

### `lb_big_words_init()`

`LB.big_words` from `NX2SARIA_LIB_PREBUILD_BIG` (SPIR-V words; `LB_BIG_WORDS` when unset, 0 no limit): the boot
(`lb_start()`) and the preparation (`lb_warm_prepared()`) read it here, so both draw the line at the same size.

### `lb_big_left_out()`

The one rule for which compute shaders are built ahead, for the boot (`lb_compute()`) and the preparation
(`lb_warm_worker()`) alike: one over `LB.big_words` only when keep says the boot keeps its pipeline (`cu_keep()`: among
the `NX2SARIA_COMPUTE_KEEP` shaders runs dispatched most recently, `compute-used.bin`). Its first dispatch builds any
other (`vk_pipeline_compute()`). The preparation once built them all into the driver's cache: on an RTX 3050 Laptop with
NVIDIA 546.18 the last three of 42,142 variants of a warm after an update took 2.5, 16 and 74 min, an hour and a half of
a two-hour update, with the PC's 16 GB of RAM nearly full, for pipelines the game never asks for.

### `lb_compute()`

The boot's build of one compute shader's pipeline. Keep 0: the pipeline is built for the pipeline cache only and
destroyed at once, unless a dispatch came to wait for it meanwhile; a shader over `LB.big_words` is then not built at
all (`lb_big_left_out()`: no run dispatched it, and a pipeline of it would hold about 100 MiB of device memory unused;
Compute pipelines the boot keeps). cs may be a copy the caller releases afterwards when keep is 0: nothing here holds
on to it then. The key is `vk_pipeline_compute()`'s.

- A shader over `LB.big_words` is built only from the caches (`compute_create()` `cached_only`); one they do not hold,
  or any of them without `P.cache_control`, goes to the background builder (`bg_queue()`) or to its first dispatch
  (`lb_defer()`).
- One the replay compiles now (`cs_flight_has()`, `vk_pipeline_compute()`) or that is entered already counts as there.
- The compile runs below normal priority: after an update or a driver update every one of these pipelines is compiled
  again (about 140 ms each on an RTX 5090's driver, minutes of two threads for a long play history), and at normal
  priority those threads took cores from the game's own on a CPU with few of them. While the shader is in flight a
  dispatch of it waits for this build (`vk_pipeline_compute()`) and raises the thread to normal: a compile left at idle
  priority under load held that frame for seconds. The dispatch may have come between the unlock and the call below,
  which would undo its raise, hence the look at the slot after it. Without a slot or a handle nobody could raise the
  thread, so it compiles at normal priority.
- A private shader cannot have a dispatch waiting on its `cs_flight` slot. Shared/kept shaders always create their
  handle so a dispatch that arrives while the build is in flight still receives the pipeline it is waiting for.
- A dispatch that waited for it makes it kept.
- A pipeline built for the cache only is not counted in `P.stats.compute`; its compile counts in `cache_merges`
  instead, or the autosave would not see what it added to `P.cache`.

### `lb_library()`

The pre-rasterization (vertex) or fragment-shader library of one shader with the LB state. keep 0: build into the
driver's pipeline cache only (sh is a private copy; the library is destroyed).

### `lb_skip_lib()`

A vertex or fragment variant with the graphics pipeline library off: no stage library to build ahead.

### `lb_worker()`

Boot: compute pipelines (the first `LB.n_keep` entered for the process, the others only built into the pipeline cache)
and kept libraries.

- `lb_me` is its index: its handle is `LB.th[lb_me]`. A variant on the skip list (`skip_listed()`) is counted and
  passed over.
- The memory gate (`gate_wait()`): without room for another build the worker waits, and after `GATE_WAIT_MS` it and the
  other workers end. What they leave is built at its first dispatch or draw, as what a run never recorded. (The play
  gate leaves this prebuild alone: The play gate.)
- A compute shader whose pipeline does not stay is read as a copy of this thread's own and released after its build:
  kept in the shader cache's memory, the 3,900 of an installed copy's list were 240 MB of the process's commit for
  shaders most runs never dispatch. A dispatch that does come reads its shader then (0.05 to 0.15 ms from the files this
  pass has just read). The shaders of kept pipelines and libraries stay loaded: their pipeline and library entries point
  at them.
- The shader is loaded at normal priority (the shader cache's lock, disk reads).
- A vertex or fragment shader is never a copy: a kept library points at it. Without the graphics pipeline library it is
  counted (`lb_skip_lib()`).
- The last worker out logs the prebuild and starts the background builder (`bg_start()`).

### `lb_state()`

The library state from the key file's records (prewarm); defaults without one.

- State index: clamp | poly (0 fill, 1 line, 2 point -> `VkPolygonMode`) << 1 | `minus_w` << 3 (pr); samples log2 (fs).
- No key file: -w..w, the depth range the game clips to (all 2706 records of a recorded key file).
- The sample count is the one most recorded pipelines use.

### The driver's cache of stage libraries, warmed again during play

The preparation (`lb_warm_prepared()`) builds every prepared variant's stage library once, so the driver's disk cache
holds it and the library a draw needs later takes a few milliseconds instead of a compile: 3 ms against 100 ms for a
vertex and 340 ms for a fragment library on an RX 7900 XT (driver 26.10.1). The driver's cache does not outlive
everything, though: a driver update empties it, the AMD driver keeps a limited amount and drops old entries, and a
change to what a library is built with (the dynamic states it lists, as when dynamic vertex input was turned off for
this driver) gives every library another entry. After any of them each pipeline a draw first needs compiles cold: the
draw is skipped for the frames that takes, or waits for it. Measured on that card with the cache cold, a walk through a
village not seen before skipped 11,180 draws while their pipelines built and 94 draws waited 5.5 s in all, one of them
1.9 s.

So every start checks: a sample of the prepared fragment libraries is built (`lw_probe()`), and when their median time
says the cache is cold, the whole prepared list is built again into the driver's cache on a few idle-priority threads
while the game runs (`lw_worker()`), as the preparation does but without stopping play. `shader-warm.txt` beside the
caches says where that stands: "warming" while it runs, so a start after a run that ended meanwhile goes on, and "done"
with the driver and a count of the times the cache was cold again right after a finished warm. At two in a row the
driver does not keep stage libraries at all and warming stops being tried (`NX2SARIA_LIB_WARM=0` turns it off; =force
warms at every start).

The check times builds, which a busy CPU only makes longer: as the guest starts beside the boot's background builds, a
sample built at below-normal priority read a warm cache as cold on an RTX 5080 (0.9.2, a median of 136 ms on 16 logical
processors), and the whole list of 42,231 libraries was built again in 873 s on 4 idle threads, although
`--warm-shaders` gave all of them back in 10.6 s and the driver's cache file did not change. So the sample is built at
normal priority, into the driver's cache only (`no_cache`: a library our own pipeline cache holds would read fast
whatever the driver has), and a sample that says cold is built again 10 s later before the cache counts as cold, except
while the start waits for the whole warm after a driver change, where cold is the expected answer. The cache counts as
warm when `LW_WARM_SHARE` of the sample was fast: a cache the driver has partly evicted (a size limit keeps some of the
libraries and drops others) is to be warmed, and one or two fast builds say little. `shader-warm.txt` also keeps how far
a warm came ("warming <driver> <strikes> <variant>"), written every 8,000 variants and when the warm ends for a run
before it is done (`lw_keep_progress()`): a run that ends before the warm does is followed by one that goes on from
there, not from the first variant again, which on a PC whose sessions were shorter than a warm under play (15 minutes)
began the list again at every start.

- `#define LW_BOOT_THREADS 32`: while the start waits for the warm (`bf_wait()`): every core, as the preparation.

`LW`:

- `volatile int32_t short_mem`: the memory gate refused a build for `GATE_WAIT_MS`: the warm ended for this run.
- `volatile int32_t checked`: `lw_main()` has returned: the workers it started, if any, are counted in running.
- `size_t from`: the variant this run's warm began at (an earlier run's progress, `lw_check()`).
- `int bg_threads`: the workers that go on beside the game (`lw_worker()`).
- `uint32_t vendor`: the GPU's vendor id, for what a cold cache's log line adds (`runtime/vk/vk_warm_hint.h`).

### `lw_stamp_at()`

at: for "warming", the variant a later run goes on from (all before it are built).

### `lw_build()`

One prepared vertex or fragment variant's library into the driver's cache; its time in ms, negative when the variant is
on the skip list, not in the shader cache or of another stage.

### `lw_keep_progress()`

A warm that ends for a run before it is done keeps how far it came, as the 8,000-variant marks do: the memory gate
stopped it (`lw_worker()`) or the window closed (`vk_pipeline_save_exit()`). Without it a PC whose runs each end a few
hundred variants in (an integrated GPU whose memory is the system's, with the commit near its limit) begins the list
again at every start and never gets past its first variants. The point kept is `LW.from` plus the variants done less
one per worker: the workers take the variants in order and each holds at most one unbuilt, so all below it are built.

### `lw_worker()`

A library warm worker; its index is its journal slot while the start waits (`bf_wait()`). It builds into the driver's
own cache only (`no_cache`). Beside the game it runs at idle priority, and of the workers the whole warm started only
the background's count (`LW.bg_threads`) go on. Each variant passes the play gate (`play_rest_wait()`), then the memory
gate (`gate_wait()`): without room the warm waits, and after `GATE_WAIT_MS` it ends for this run; `shader-warm.txt`
keeps saying "warming", so the next start goes on with it (`lw_keep_progress()`). From the journal mark to its clearing
(`journal_mark()`), a process that dies names this variant. Every 8,000 variants the point reached is written, as in
`lw_keep_progress()`. The last worker out says how the warm ended, and writes "done" when it ran to the end.

### `lw_time_sample()`

The median time of `LW_SAMPLE` prepared fragment libraries spread over the list, in ms, and in `*fast` how many were
under `LW_COLD_MS`, in `*got` how many were built; negative without half of them. The list is cut in `LW_SAMPLE`
steps, from a start the clock picks, and the first fragment variant of each step is built (`lw_build()`).

### `lw_warm_sample()`

1 when the sample says the driver's cache has the prepared libraries.

### `lw_probe()`

`lw_time_sample()` at normal priority, and once more 10 s later when it says cold, unless the start waits
(`bf_waiting()`). The better of the two counts. The thread goes back to below normal afterwards when the game runs
beside it.

### `lw_say_cache_folder()`

One line before the sample: the folder NVIDIA's driver keeps its shader cache in and what it holds, so a log that finds
the prepared libraries gone says whether the files were gone from the disk or were there and did not match (the
launcher's log says the same around an update, for every driver's folder). It is the folder the environment names, which
is the game's own beside the exe unless the player named one (`runtime/vk/vk_device.c` `nvidia_cache_beside_exe()`); the
driver's default folder is not in the environment, and without the variable there is no line.

### `lw_check()`

The check on its own thread (`lw_main()`), below normal priority unless the start waits. Its libraries go into the
driver's cache only, as the workers' do (`no_cache`).

1. `shader-warm.txt` is read: state, driver, strikes and the variant reached (a line without the count starts from the
   beginning).
2. A warm an earlier run left at "warming" on this driver is resumed, not sampled: the variants the earlier run built
   are in the driver's cache and the rest are not, so a sample would read the cache as warm or cold by how far it came.
   It goes on from where it stopped.
3. Otherwise the sample decides (`lw_probe()`). Warm: when the line had strikes on this driver, the driver kept them
   this time and the count goes back to 0. Cold again after two finished warms on this driver (strikes 2): the driver
   does not keep them and no warm is tried, unless `NX2SARIA_LIB_WARM=force`, which also warms a warm cache. A cold
   cache right after a finished warm adds a strike, and its log line says more than a first cold
   (`runtime/vk/vk_warm_hint.h`: `LW_COLD_AFTER_WARM`, `LW_COLD_FIRST`, `LW_WARM_FORCED`).
4. The workers start (`lw_worker()`): a quarter of the logical processors, 2 to `LW_MAX_THREADS`, beside the game; every
   one up to `LW_BOOT_THREADS` while the start waits.

### `lw_start()`

Boot, once the library state is known (`lb_state()`): the prepared list (`shader-prepared.bin`, its records alone,
whichever version the file is) is read and the check starts on its own thread (`lw_check()`). Nothing without the
graphics pipeline library, with `NX2SARIA_LIB_WARM=0`, or without a prepared list (a build without the preparation
step). Once a process.

### `lb_start()`

Boot: compute shaders of the warm list (recorded and prepared), those whose pipeline stays (`cu_keep()`) first: they are
the ones the game is about to dispatch; then the first `NX2SARIA_LIB_PREBUILD_KEEP` recorded graphics variants (play
priority 0). Before them it starts the recorded mesh pipelines' libraries (`mesh_pre_start()`), sets the libraries'
state (`lb_state()`, also for `lw_start()`), and starts the library warm (`lw_start()`), or with the whole warm leaves
it to `bf_wait()` after the compute pipelines (`BF.lw`). With `NX2SARIA_LIB_PREBUILD=0` none of it starts;
`NX2SARIA_LIB_PREBUILD=N` above 1 caps the list at N, 1 is plain on.

Two threads by default (1 under 4 CPUs), not half the CPUs; `NX2SARIA_LIB_PREBUILD_THREADS` sets the count, at most
`LB_MAX_THREADS`, at idle priority (a cold driver takes ~2 s per large compute shader). On a 32-thread CPU with NVIDIA's
driver, 16 builders kept the driver's lock busy: 77% of the process's running time was a spin loop in `nvoglv64.dll`
(about 8 cores), the prebuild had not finished after 34,000 frames, and `NX2SARIA_LIB_PREBUILD=0` took it to 5% and the
game to its frame limit (RTX 4080 SUPER, driver 617.14, 2026-11-08). With the driver's disk cache warm
(`--prepare-shaders`, `--warm-shaders`) a build takes well under a millisecond, so two threads still finish the list
quickly. The two take the shaders dispatched most recently (e[0..`n_keep`)); one goes on with the rest (`lb_next()`),
which is only built into the caches. Where the rest still runs during play, two builders there cost frames: one against
two for the whole list, one 20 s route each on an RTX 5090, Castle Town 76.1 against 73.7 fps with 1 frame over 25 ms
against 6, Hyrule Field 85.7 against 80.7 fps. `NX2SARIA_LIB_PREBUILD_THREADS` keeps its count for the whole list.

The whole warm (`bf_wait()`): no game runs beside these builds until they are done, so every core takes them (up to
`LB_MAX_THREADS`), as in the preparation; should the game start first, the count above goes on (`lb_next()`).

### `lb_stop()`

Stops and joins the library warm (`lw_stop()`), the mesh library builder, the prebuild workers and the background
builder, the last after its current build, if one runs.

### `bf_time_up()`

1 once the whole warm's time on the boot screen is up (`BF.t_end`): the builders go on as on any other start.

### `bf_wait()`

The whole warm's wait (see `bf_decide()`), on the prewarm thread after the recorded pipelines: the compute
pipelines `lb_start()` has just started, then the library warm. The counts go to the boot screen
(`vk_pipeline_prewarm_progress()`). It ends with the warm, at the warm's time limit, or when the boot stops waiting
(`vk_pipeline_prewarm_detach()`); in the last two the builders go on beside the game. The time may be up before it
starts: the recorded pipelines took all of it.

The library warm's journal is open while the start waits (`journal_open()`) and closed when the warm is over or the
game runs (`journal_close()`). `lw_start()` runs either way: when the wait is over, the check and the warm run beside
the game as on any other start. The count on the screen is a resumed warm's rest (`LW.n - LW.from`). Unless the process
is stopping, `boot-warm-owed.txt` then says "done": the boot screen's turn is over for this GPU and driver
(`runtime/vk/boot_warm.h`). Nothing waits from here on: later builders are the background's.

### `pw_mark()`

What each preparation worker hands the driver now (under `LB.stat`): the variant's index + 1 (0: none), since when, and
its SPIR-V length. `lb_warm_prepared()` reads it to stop waiting for builds that do not end (`prep_stalled()`).

`PW`:

- The first `PW_OUT` large compute shaders the preparation left out (`lb_big_left_out()`), named in its last lines.
- `int test_ms`: `NX2SARIA_PREPARE_STALL_TEST_MS`: the last variant waits this long first, a stalled build for tests.

### `lb_warm_worker()`

Preparation: one prepared variant at a time into the driver's disk cache (private shader copy, nothing kept). arg is the
worker's journal slot. The variant is marked (`pw_mark()`) from the hand-out on, so `prep_stalled()` never takes a
worker loading one for idle; once its shader is loaded the journal names it (`journal_mark()`) until the build returns,
so a crash names this variant. A large compute shader the boot would leave out (`lb_big_left_out()`) is left out here
too, its first dispatch builds it, and the first `PW_OUT` of them are kept for the last lines. Every 4,000 variants the
counts are logged (`lb_report()`).

### `prep_stalled()`

The preparation's wait for its workers, every 2 s: once every variant has been handed out, and every build still running
has been in the driver's hands for `stall_ms` or more (`NX2SARIA_PREPARE_STALL_S`), the wait is given up. A Vulkan
create cannot be cancelled, and on NVIDIA 546.18 a few compute shaders took 16 and 74 min each after the other 42,139
variants were done in 28 min, while the launcher's update showed "built 42141 of 42142". So:

- Each build still running is named with its time (the variant's input id, as `shader-prepare-skip.txt` names one);
- `shader-warm.txt` says where the game goes on (`lw_check()`): "warming" from the first unfinished variant when one is
  a vertex or fragment library, so a later start builds the rest on idle threads beside play (the variants already built
  come back from the driver's cache in a few ms); "done" when all are compute shaders, which every boot builds ahead
  anyway (`lb_compute()`);
- The journal goes first, so the next start does not take the builds it names for a driver crash (`skip_init()`);
- The process ends with code 0 (`plat_exit`): the step did its work, and `tools/make_game.py` says the rest goes on in
  the game. What the driver finished is in its disk cache already; the pipeline cache was saved before the libraries
  (`seed_pipelines()`). k: the workers; jp: the journal's path.

A build still loading its shader is named without a length. The journal is closed under `SK.jlock`, so a worker that
returns now marks nothing more (`journal_mark()`). The count of built variants leaves out those that build nothing
(skip list, no shader, no library, large ones left out).

### `prep_sample_warm()`

`--warm-shaders`, before the whole list: `LW_SAMPLE` prepared fragment libraries spread over it, built into the driver's
own cache alone (`no_cache`, as the library warm in play builds) and timed, as the game's check at every start does
(`lw_check()`): the first fragment variant of each of `LW_SAMPLE` steps of the list, from the middle of the first.
1 when at least half of them were built and `LW_WARM_SHARE` of those came from the driver's cache. `*fast` and `*got`:
the counts. Each is journalled in slot 0 (`journal_mark()`): a driver that dies on it names it, as the full pass would.

### `lb_warm_prepared()`

Preparation: every variant of `<dir>`/`shader-prepared.bin` on every core at normal priority. sample: `--warm-shaders`
after an update, which first asks the driver's cache (`prep_sample_warm()`) and builds nothing more when it has them. It
took 10 to 20 s on a player's PC at every update, all of it 42,000 libraries the driver gave back at once, and it still
takes the minutes of a full build when the driver no longer has them (a driver update, or a change of what they are
built with: 380 s on that PC once). `NX2SARIA_LIB_WARM=force` builds the whole list whatever the sample says.

- The list is the records alone, whichever version the file is. The skip list is read first (`skip_init()`, which
  removes a leftover journal before this run's is created), then the boot's line for large compute shaders
  (`lb_big_words_init()`) and its record of the dispatched ones (`cu_open_prep()`). The library state is the default
  one (`lb_state()` without records).
- `shader-warm.txt` (`lw_check()`) says "warming" with this driver while a warm the game began in play has not come to
  the end of the list. The variants before the point it reached are in the driver's cache and the rest are not, and the
  sample, spread over the list, finds about that share fast: at 31,996 of 42,239 it found 9 of 12, which is
  `LW_WARM_SHARE`, and read the cache as warm. `--warm-shaders` built nothing, and the next start went on warming the
  rest on idle threads during play (10,243 of 42,239 variants left on an RTX 5080, with the replay at 20-39 ms a frame
  in Castle Town meanwhile). An unfinished warm is built here whatever the sample says, and a preparation that builds
  the whole list marks the warm done, so the game does not start it again.
- Every 2 s a count on stdout for `tools/make_game.py` (`step_shaders`), as `runtime/gfx/gfx_shader.c` `prepare_files()`
  prints for the translation: a driver building for a long time still shows the step moving.
- `prep_stalled()` checks every 2 s, and does not return when it gives up the wait.
- When every build returned the journal goes, and with the graphics pipeline library `shader-warm.txt` says "done": the
  whole list is in the driver's cache.
- Without the graphics pipeline library only compute variants are built: the last line says so, rather than count the
  skipped ones as built. Further lines name the variants of the skip list and the large compute shaders left out.

### The shared pipeline list

`tools/pipeline_list.py`; `seed-pipeline-keys.bin` next to the game.

`tools/prepare_shaders.py` writes the list's key records there, in the layout of `pipeline-keys.bin`, after putting the
shader variants they name into the seed. The preparation builds them once, after the seed is translated: each pipeline
goes through the pipeline cache and so into `pipeline-cache.bin` and the driver's own disk cache, and is dropped with
the process. Nothing is added to this folder's `pipeline-keys.bin`; every boot then counts the list as recorded
pipelines beside the install's own (`keys_open_seed()`) and builds them before play, from those caches.
`NX2SARIA_PREPARE_PIPELINES`: all (default) builds every record; mesh, the list's mesh pipelines only (and every record
without the graphics pipeline library); 0, none. A vertex pipeline's module depends on the vertex output union of the
pairs known (`vs_outs()`): here the list's, as at the first boot of a new install, which reads the list and none of its
own yet, so a vertex pipeline built here is the one that boot asks for. An install that has drawn more pairs since may
need some of them again with wider outputs; the boot builds those. Without the graphics pipeline library (`vk_caps_gpl`:
AMD's Windows driver for RDNA 2 and older) a vertex pipeline's outputs come from its own pair (no independent layouts),
so a listed one always is the one a draw asks for. A mesh pipeline, which has no stage libraries, otherwise leaves its
draws out of the picture for its whole compile (0.2 to 1.1 s each on a first visit of a forest, 18 of them). Where mesh
pipelines have stage libraries (`mesh_libs()`) the listed ones are still built whole, and the libraries of their stages
are built after them (`mesh_pre_run()`), for the mesh pipelines no list names. A driver whose compiler crashes on one of
these would crash at every preparation: `seed-pipelines-running.txt` marks the step, and a run that finds the marker of
a run that died skips the step for good on this folder (the pipelines are then built at their first draw, as without a
list) until the marker is deleted.

### `seed_pipelines()`

The preparation's build of the shared list (The shared pipeline list). Returns 0 (nothing to say without a list).

1. The list's shaders are translated or loaded (`gfx_shader_prewarm()`, as `prewarm_run()`), then the marker is
   written.
2. Each record is entered (`pipe_miss()`, mode 2), as at the boot: a vertex pipeline gets its vertex output union
   first (`vs_outs()`). `K.t0` and `K.prep` set the start of `prebuild_report()`: this mode never runs
   `prewarm_run()`, which sets them in play. A mesh pipeline on a GPU without mesh shaders or with the mesh fallback is
   not built: no mesh draw uses one.
3. Every one is built (`K.until` 0: no boot waits for this), and no game runs beside it, so the memory gate
   (`gate_enter()`) refuses nothing (`GATE.off`). The first is built alone (`warm_builder()`); it waits for those a pool
   worker took.
4. The listed mesh pipelines' stage libraries (`mesh_pre_run()`), before the save and inside the marker: the file then
   holds them, and a driver that dies on one is met by the next preparation as one that died on a listed pipeline.
5. The pipeline cache is saved (`cache_save()`, waiting up to 5 s for a moment with no build running), and the marker
   goes.

### `prep_card_line()`

The first line a preparation mode prints once it has its device: the card it builds the graphics driver's cache for, and
by what the settings chose it (`runtime/vk/vk_device.c` `pick_device()`, which the game takes its card with too;
`runtime/vk/gpu_choice.h` `gpuc_prepare_line()`). The launcher's log keeps every line of the step, and the device lines
the choice also logs are among sixty others there, so a report of a slow update, or of one that built for the wrong
card, is answered from this one.

### `vk_pipeline_prepare_pipelines()`

`nx2saria.exe` `--prepare-pipelines`: the list's pipelines alone, for a folder whose shaders are prepared already.

Like the other preparation modes it sets `prep_mode`: builds go into `pipeline-cache.bin` alone (`cache_load()`).

### `vk_pipeline_prepare_shaders()`

`nx2saria.exe` `--prepare-shaders` (`tools/prepare_shaders.py`): the device sets the translator's host answers (they are
in every cache key), the seed next to `nx2saria.exe` is translated on every core, then every prepared variant's stage
library or compute pipeline is built once into the driver's disk cache (`lb_warm_prepared()`). No game runs.

- `prep_mode`: builds go into `pipeline-cache.bin` alone (`cache_load()`).
- `NX2SARIA_PREWARM_XLATE` translator processes, half the CPUs and at most 16 when unset.
- The listed pipelines before the stage libraries: the pipeline cache is saved with them in it and without the tens of
  thousands of libraries the next step builds, which would take the file past its size limit.

### `vk_pipeline_warm_shaders()`

`nx2saria.exe` `--warm-shaders`: every variant of `shader-prepared.bin` built into the driver's disk cache again from
the shader cache, without translating the seed. The AMD cache is keyed to the exe path (a relink keeps it: 29,587
variants in 1.8 s) and holds a limited amount: preparing another build dir evicted this one's libraries, and a library a
draw needed then took 40-220 ms in play instead of 0.2 ms. Seconds when the libraries are still there.

It sets `prep_mode`, as `vk_pipeline_prepare_shaders()` does, and its card line begins "prepare-shaders" too: the lines
the update's step reads all begin so, whichever of the two modes prints them. The listed pipelines are built before the
libraries (`seed_pipelines()`), as in `vk_pipeline_prepare_shaders()`; then `lb_warm_prepared()` with the sample.

### `vk_pipeline_probe_prewarm()`

`nx2saria.exe` `--probe-prewarm`: the boot's shader and pipeline stages without the game, from the caches next to
`nx2saria.exe` (`pipeline-cache.bin`, `pipeline-keys.bin`, `shader-answers.bin`, the shader cache): the pipeline cache
load, the shader prewarm, the recorded pipelines and the compute pipeline prebuild, each followed by
`vk_mem_stage_log`'s line, so the log tells what every stage leaves in device memory and in the process's commit. Then
up to `PROBE_DISPATCHES` compute shaders whose pipeline the boot did not keep are asked for as a dispatch would
(`gfx_shader_load_input` for the shader, then `vk_pipeline_compute()`), which gives what a first dispatch costs on this
machine and how many of them the pipeline cache held. Fails when one of those pipelines is not created, when the
prebuild kept a pipeline it should not have, when a large compute shader went to the background builder although no run
dispatched it, or when the record of dispatched shaders, written to a file of the probe's own and read back, does not
hold the probe's dispatches. `compute-used.bin` itself stays as it is; the pipeline cache is saved as after a boot.

The prewarm (`vk_pipeline_prewarm()`) brings the device up itself: it has to own the key file before `ensure_init()`
runs. A dispatch never waits in this mode, so no more than `LB.n_keep` pipelines may have been kept, and only a shader a
run dispatched (one of those) may be queued for the background builder. The probe's dispatches load their shader from
the disk (the boot released its copy) and leave out the large ones, whose first dispatch compiles them: not what is
timed here.

### Dynamic state

### `vk_pipeline_set_dynamic()`

`vk_pipeline_set_dynamic_delta()` with nothing known: every call is recorded.

### `vk_pipeline_set_dynamic_delta()`

Every graphics pipeline the replay binds lists the same dynamic states (`P.dyn`; mesh pipelines leave out topology,
primitive restart and vertex input), so a value recorded into a command buffer holds across pipeline binds until it is
set again. Each call is recorded only when its value differs from the one `*sh` says the command buffer has: a new
pipeline usually changes the shaders and a field or two, not the twenty states this used to record with every bind.

- The front face maps unchanged: the game sets the upper-left window origin, and the y-flipped viewport (`y_flip`) puts
  NDC +y at row 0 as NVN does, so the framebuffer winding Vulkan tests is the one NVN tests (a lower-left origin would
  need it mirrored). The cull mode too: `NVN_FACE_*` has `VkCullModeFlagBits`' values.
- The depth bias enable is the `NVN_POLYGON_OFFSET_ENABLE_*` bit of the polygon mode in use: point 1, line 2, fill 4.
- Vertex input not recorded (no dynamic vertex input) is marked unknown to the next draw.
- Alpha to coverage is recorded through the depth clamp call's signature: the two have the same.
- The blend calls write targets [0, nt); targets past nt keep what the command buffer had.

Returns the calls recorded; `*skipped` adds those left out.

### Statistics, persistence, teardown

### `cache_save_one()`

Copies a pipeline cache's data into its file, with `cache_rw` held exclusively for the copy only (not the file write):
`P.play` into `pipeline-cache-play.bin` when there is a play cache, else `P.cache` into `pipeline-cache.bin`; and
`P.cache` once, with or without a play cache, after the play file was merged into it at the start (`P.fold`), which then
removes the play file, and at the exit of a run that started without `pipeline-cache.bin` (`P.base_at_exit`;
`runtime/vk/vk_cache_stamp.h` `cache_save_writes_base()` chooses).

Only when the data changed: the size of a cache's data is asked first, and a cache whose data has the size its file
holds is neither copied nor written. A pipeline cache only gains entries, so the same size is the same data. Before,
every save copied and wrote the whole cache whenever a pipeline had been created since the last one, also when the cache
had held it: three saves of 358.1 MB in one run that added 0.3 MB (2026-11-11).

The lock is only ever tried, never waited for: a waiting exclusive request would hold back every new build (a shared
request queues behind it), so a pending autosave could stall the replay. The save at exit (exiting) holds new builds
back instead (`cache_saving_at_exit`) and so only waits for the builds already running. `wait_ms`: how long to keep
trying while builds run (0: one try). Returns 0 when no build-free moment came, -1 when the data could not be copied or
its file not written (the log says which), 1 otherwise (saved, or nothing to save).

The driver writes the data straight into the new file, mapped (`plat_file_swap_begin`), not into a buffer that is then
written out: a buffer the size of the cache was committed for every save, 450 to 560 MB about every 20 s while new
pipelines were built, on top of a process that already took the system's commit to its limit. Each save raised the
process by 480 to 1490 MB for its length, the commit left fell to 17 MB, and two runs lost the Vulkan device in the
middle of a save (their .tmp files stayed behind, cut short). The mapped pages belong to the file and take no commit.

- `static PlatMutex save_lock = PLAT_MUTEX_INIT`: one cache save at a time; shutdown takes it to destroy the cache.
- `#define SAVE_SLOW_MS 1000`: a save's copy or write this long or longer is logged with the time it ended.
- A cache past the limit is not written: the next start would drop the file whole (`cache_data_read()`), so the file
  keeps its last save under the limit and only the pipelines made since then are compiled again. A preparation writes
  anyway: what it builds is the base every start reads, and a file it cannot write would leave none.
- With the size just asked for, under the lock that keeps builds out, the second call returns everything. Should a
  driver return `VK_INCOMPLETE` all the same, the file is not replaced.
- The held-back builds go on before the log line and the file write: one of them may hold a lock those wait for.
- A save that took `SAVE_SLOW_MS` or more, in the copy of the driver's data or in the file's write, says when it ended,
  to line it up with the system's journal: a session of 2026-11-21 had one of 5.2 s among stalls of 1 to 3 s, and the
  line could not say which came first.
- A written `pipeline-cache.bin` after a fold removes the play file: its content is in `pipeline-cache.bin` now.

### `cache_save()`

Both caches' saves (`cache_save_one()`): 1 when each is done, 0 when one found no build-free moment, -1 when one failed.

- Not under `P.lock`: holding `P.lock` through the copy and the file write (about 60 ms on the 1.4.3 fight) stopped
  every pipeline lookup of the replay, a freeze at each 20 s autosave.
- No cache after `save_lock` is taken: `vk_pipeline_shutdown()` destroyed it.
- `pipeline-cache.bin`: at every save without a play cache, once for a fold, and at the exit of a run that started
  without it (`P.base_at_exit`); the play file beside it as usual.
- While a fold is not written yet (`P.fold`: refused at the limit, or no build-free moment) the play file still holds
  what was folded in memory only: written over with this run's play cache, that would be in neither file.
- The answer is the worse of the two.

### `save_with()`

A save with its statistics: it flushes the key records, the pipeline binaries, the compute record (`cu_flush()`) and
the first dispatches' line (`cf_log()`), logs the graphics pipeline library's counts and the play gate's (`pg_log()`),
then saves (`cache_save()`) and returns its answer. The mesh pipelines' line counts them by themselves: with stage
libraries the first two numbers follow each other and the times are a link's; without (the start's line says why) none
is linked and the time is a whole build's. Without `P.cache` the pipeline binaries' line (`pb_log()`) is written here;
with it, it comes with its own save.

`SAVE_WAIT_MS`: builds still running after this long at a save that waits (the exit, `vk_pipeline_save()`) leave the file
as it was.

### `vk_pipeline_save()`

The save at shutdown (`vk_pipeline_shutdown()`), after `--probe-prewarm` and after the self-test: it waits up to
`SAVE_WAIT_MS` for a moment with no build running. The preparation modes save their own way (`seed_pipelines()`).

### `vk_pipeline_save_exit()`

The save as the process ends (window closed, the game's exit, a timed run's end): new builds wait while it waits up to
`SAVE_WAIT_MS` for the running ones to finish (`cache_saving_at_exit`).

- The boot's journal is closed first (`journal_close()`): an orderly end during the whole warm is not a driver crash.
- The process ends without joining the library warm's workers: a warm still under way keeps how far it came. They stop
  taking variants first; one that finishes its build before the exit writes the mark again, a little further.

### `cache_sizes_ask()`

The memory report's figure for the pipeline caches the driver keeps in process memory (`vk_pipeline_mem()`): the size of
each cache's data, asked once a minute here on the autosave thread, and by every save (`cache_save_one()`). The size is
asked as the save asks it, at a moment no build runs into either cache (`cache_rw`, tried once and never waited for),
and under `save_lock`, which the shutdown takes before it destroys the caches. The play file holds what this run
compiled, but `pipeline-cache.bin` is not written in play (only at the exit of a run that started without it): its cache
grows in memory with the compute pipelines the boot builds for it alone, and only this query says by how much.

### `autosave_once()`

The game never shuts the device down (`runtime/boot.c` and the window end the process), so the cache is saved while it
runs: at most every 20 s (`SAVE_EVERY_MS`), and only after new pipelines were built or while a save that did not happen
is still owed (`runtime/vk/vk_save_retry.h`). New pipeline keys are appended every 2 s, with the pipeline binaries and
the compute record.

Each tick it also destroys the pipelines given back earlier that the GPU is done with (`release_drain()`), lets the
pipelines no draw used go while video memory is short (`pressure_release()`), and asks the caches' sizes once a minute
(`cache_sizes_ask()`). The count that makes a save due adds `cache_merges` to the pipelines built: a merge comes after
its pipeline was counted (`bg_main()`). `P.fold` also makes it due: the play file merged at the start is to be written.
The save is made only at a moment with no build running (`cache_save()`): while one runs it waits for the next tick,
500 ms on, before it logs or flushes anything, so a busy stretch does not repeat the statistics lines every tick.

### `autosave_main()`

On its own thread: the save (`vkGetPipelineCacheData` and the file write, about 60 ms) and the key appends (a flush to
the disk each) ran on the present thread, and the render thread waited for it to take the next frame.

### `vk_pipeline_shutdown()`

Stops the background builders (`lb_stop()`), drains the workers (`workers_drain()`: jobs point at layouts and Pipe
entries freed here), saves, and destroys the pipelines, stage libraries, layouts and caches. The caches go under
`save_lock`: not while the autosave thread copies one.

### Self-test

Census shaders (`probe_census_dir`, `tools/shader_census.py`) through `gfx_shader_get` into pipelines.

Graphics pairs are a fragment shader and a vertex shader of the same source file, spread over the census; compute
shaders are spread the same way. Every pipeline is built twice (the second call must hit the cache) and the dynamic
state of the first graphics pipeline is recorded into a throwaway command buffer.

### `census_shader()`

The program the game would hand NVN for a census entry, translated with default answers (as
`runtime/gfx/gfx_shader_probe.c`).

### `selftest_state()`

A state for a census pair: one target per fragment output (RGBA8 of the output's type), D32S8 depth with the test on,
alpha blending on target 0, one RGBA32 attribute per vertex input, interleaved in stream 0.

### `selftest_record()`

Records the dynamic state of a pipeline into a command buffer that is never submitted (validation checks the calls).

### `vk_pipeline_selftest()`

The census folder is `build/v<version>/shaders` (`probe_census_dir()`). Candidate pairs: each fragment shader of a
source file with one of the file's vertex shaders, round robin; the census is sorted by stage within a file, vertex
shaders first. Up to 64 pairs and 16 compute shaders are spread over the census. Passes when at least one of each is
built, every second call hit the cache, the dynamic state was recorded and validation reported no error.

### `vk_pipeline_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): the pipeline table's entries with their pipelines and fast
links (only `pressure_release()` takes entries out, while video memory is short), layouts, stage libraries, the job
queues and the `pipeline-keys.bin` set. A walk of the hash chains under `P.lock` held shared: the workers' inserts and a
draw's memo miss wait for it (well under a millisecond).

### `vk_pipeline_mem()`

The memory report's pipeline figures (`runtime/vk/vk_memory.c` `outside_log()` and `system_log()`), once a report: what
holds device memory outside VMA, counted because the driver gives no size for a pipeline, and the host memory of the
tables. A walk of the hash chains under `P.lock` held shared, as `vk_pipeline_sizes()` walks them (well under a
millisecond: the draws' lookups wait for it). A graphics entry's pipeline is whole (built in one piece, no fast link
before it), fast-linked (from its stage libraries, the optimised build not done or failed) or optimised (the whole build
that replaced a fast link). Not used lately: no lookup since `idle_min` wall minutes (`pipe_used()`), never: none in
this run (the boot's prebuild made it from the key file or the shared list). Stage libraries are counted by kind: every
library key starts with its `LIB_*` kind.

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
