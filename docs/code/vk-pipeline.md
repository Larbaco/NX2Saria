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
1) in the translator (tools/xlate/patches/0006).

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

- `enum { NP_16 = 1, NP_32 = 2, NP_64 = 4 }`: float widths in `P.nan_preserve` and `spirv_nan_preserve()`.

`P`:

- `int indep`: stage-independent layouts (`vk_caps.gpl`): vertex set 0, fragment set 1.
- `int push_stage`: `GFX_STAGE_*` whose set is pushed with independent layouts.
- `int gpl`: draws whose pipeline is building get a fast-linked one (`vk_caps.gpl`).
- `int mesh_lib`: mesh pipelines are linked from stage libraries too (`mesh_lib_init()`, `mesh_libs()`).
- Mesh pipelines' share of the counts above: `m_links` fast links and their link time; of the `ready_mesh` pipelines a
  draw missed, `m_ready_fast` became usable through a fast link, `m_ready_ms` after the miss in all; `m_lib_*`: [0]
  their task and mesh libraries, [1] their fragment shader libraries (`lib_build()`).
- `int vs_outs_all`: `NX2SARIA_VS_OUTS`: 0 union of paired fragment inputs, 1 (all) all 32 output locations.
- `int wide_lines`: the device draws lines of other widths than 1 (`vk_pipeline_line_width()`).
- `float line_w[2], line_gran`: its range of widths and their step.
- `int cache_control`: `pipelineCreationCacheControl`: a create can ask for the pipeline caches only.
- `int feedback`: `NX2SARIA_PIPE_FEEDBACK`: 1 count pipeline cache hits, 2 also log each compute and graphics build.
- `uint64_t cache_loaded`: bytes of `pipeline-cache.bin` the driver took at boot.
- `VkPipelineCache play`: what this run compiles (`pipeline-cache-play.bin`); `VK_NULL_HANDLE`: all goes into cache.
- `uint64_t play_loaded`: bytes of `pipeline-cache-play.bin` the driver took at boot.
- `uint64_t saved[2]`: the data size of cache [0] and play [1] that their files hold; 0: not known.
- `volatile uint64_t held[2]`: the data size of cache [0] and play [1] when last asked (`cache_save_one()`,
  `cache_sizes_ask()`); 0: not asked yet (the memory report takes the loaded size).
- `int fold`: the play file's content was merged into cache: saving cache removes the play file.
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

- `int building`: a background job owns info.pipeline (under `P.lock`).
- `struct Job *job`: that job while building.
- `const struct Layout *lay`: graphics: the layout it was entered with.
- `VkPipeline fast`: GPL: the fast-linked pipeline, until the optimised one replaces it (`fast_retire()`).
- `uint8_t link_queued`: a waiting draw queued its fast link to the workers (LibJob p), not yet run.
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
served (`runtime/vk/vk_exec_draw.c`, PM) keeps an older minute: at most the one pipeline each replay context remembers.

### `key_hash()`

Hash of a pipeline table key, 8 bytes per step: a draw hashes its whole GKey, and byte-wise FNV over it cost more than
the lookup. Only the in-memory table uses it; the key file keeps fnv.

- `static __thread uint64_t mm_hash`: FNV-1a of the module `make_module()` last created on this thread (`PIPE_FEEDBACK`
  2).

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

`pb_seek`: 64-bit file offsets: long is 32 bits on Windows, and a file over 2 GB read as cut short.

- `#define PB_HEAD (4 + 4 + PB_KEY)`: a record's bytes after its size field and before its payload.
- `typedef struct { uint64_t h, off; uint32_t size; } PbSlot`: off: file offset of the record's size field.

`PB`:

- `PlatMutex lock`: the index, the pending records and rf.
- `FILE *rf`: reads records.
- `uint8_t *pend`: records not written yet; they follow the file's end in this order.
- `uint64_t file_size`: bytes on disk.
- `int no_capture[3]`: kind (`pipe_create()`) whose create refused the capture flag.
- `static __thread int pb_last`: 1: the last `pipe_create()` on this thread made its pipeline from binaries.
- `static __thread int cc_hit`: `compute_create()`'s last pipeline on this thread: 1 the pipeline cache held it
  (`P.cache`: read from `pipeline-cache.bin`, or built into it earlier in this run) or it was made from pipeline
  binaries, 0 not, -1 the driver did not say.

`own_cache`: A pipeline cache of this thread's own that its creates build into in place of `P.cache`; `VK_NULL_HANDLE`
(every thread but the background builder of large compute shaders) for `P.cache` itself. A create into it does not hold
`cache_rw`, so a build of minutes does not keep `pipeline-cache.bin` from being saved; `bg_merge()` moves its content
into `P.cache`.

`no_cache`: 1 on a thread whose creates take no pipeline cache of ours at all (the library warm in play, `lw_worker()`
and `lw_probe()`): what it builds is wanted in the driver's own disk cache only. Built into `P.cache`, the 37,572 stage
libraries of a warm grew it by 1.6 GB (2026-11-11: a save of 2026.7 MB in play, +1612 MB since the boot); the file was
then over `NX2SARIA_PIPELINE_CACHE_MAX_MB`, so the next start dropped it whole and compiled the pipelines it had held.

`cache_merges`: New content in `P.cache` that `P.stats` does not count, for `autosave_once()` to save: `bg_merge()`'s
merges, and the compiles of pipelines built for the cache only (`lb_compute()` keep 0).

### `pb_find()`

Caller holds `PB.lock`.

### `pb_insert()`

Enters a record (a hash match is taken as the key: 64 bits). 0 when one with the key is there already or out of memory.
Caller holds `PB.lock`.

### `pb_rewrite()`

An empty file with this driver's header.

### `pb_index()`

Indexes the records of f (len bytes) after its header: per record its type, key and offset. Returns the offset after the
last whole record, which is len unless the file ends inside a record (an append the process did not finish) or holds a
malformed one.

### `pb_keep()`

The length of the file at path (len bytes) to go on with after `pb_index()` found its whole records ending at good: len
when they fill it. When it ends inside a record (the process ended during an append, or a disk filled) or holds a
malformed one, the records from there on are dropped and the file is cut back to good, so that appends go on after the
whole records before it. 0 when it cannot be cut, and the file starts empty.

### `pb_init()`

Opens `pipeline-binaries.bin` and indexes its records (`cache_load()`, before `P.cache`).

- A file that ends inside a record (the process ended during an append, or a disk filled) or holds a malformed record:
  the whole records before it stay, and the file is cut back to them so that appends go on from there. Only when it
  cannot be cut does it start empty.
- `if (why || !f)`: start empty: the index is dropped and the file rewritten.

### `pb_read()`

The payload of the record of (type, key), malloc'd (`*size` bytes), or NULL. Caller holds `PB.lock`.

- `if (s->off >= PB.file_size)`: still pending, at its place in pend.
- `} else`: rf is NULL only after `pb_flush()` could not open the file again.
- `if (ok && t == type && k == ks && !memcmp(head + 8, key, ks))`: the key itself, not only its hash.

### `pb_load()`

The binaries stored for pipeline key k, created in bins (`*n` of them). 0 when none are stored or they fail.

### `pb_put()`

Appends one record to pend and the index. Caller holds `PB.lock`.

### `pb_capture()`

Stores the binaries of pipeline p (created with its data captured) under pipeline key k, then frees the data. A binary
whose key is stored already is not stored again.

- `for (uint32_t i = 0; ok && i < nb; i++)`: each binary's key, and its data unless stored already.
- `plat_mutex_unlock(&PB.lock)`: a 0 from `pb_put()`: another thread stored the same binary meanwhile.
- `if (full) pb_flush()`: a cold boot captures thousands before the autosave thread runs: keep the buffer small.

### `pb_flush()`

Appends the pending records (the autosave thread every 2 s, and the save at exit).

- Part of the append may be in the file (a full disk): the file is cut back to `file_size`, where the index places the
  pending records, or the next append would land after those bytes. The read handle is closed meanwhile, since
  `plat_file_truncate` opens the file alone.

### `cache_build_enter()`

Takes `cache_rw` shared for a build into `P.cache`; while the save at exit waits for its copy, first waits for that.

`cache_rw`: Builds into `P.cache` against reads of its data. Every create with `P.cache` holds this shared
(`pipe_create_in()`); a save holds it exclusively for the copy of the data and only takes it when no build is running.
The Vulkan rules allow reading a cache created without `VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT` while
pipelines are built into it, but Intel's Windows driver (101.8991 on an Arc 140V) crashed inside
`vkGetPipelineCacheData` during the copy, reading through a null entry, with compute and library builds running beside
the autosave. So no copy is made while a build runs, on any driver.

`cache_saving_at_exit`: Set while the save at exit waits for `cache_rw`: the builds already in the driver finish, and
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

- `static int prep_mode`: a preparation mode (set before `ensure_init()`): no play cache.

### `pipe_create_in()`

- `uint64_t t0 = perf_mark_begin()`: `NX2SARIA_TRACE=perf`: the create on any thread, its wait for `cache_rw` included.
- Not in `P.cache` nor in the driver's own cache: from `P.play`, or compiled into it. A caller that asked for cached
  pipelines only (its own `FAIL_ON_PIPELINE_COMPILE_REQUIRED`) gets the play cache's answer.

### Pipelines of a library that draws on our device

FSR 3.1: `runtime/vk/vk_ffx.cpp` `vk_ffx_proc_addr()`.

Such a library creates its pipelines itself and passes no pipeline cache, so nothing of ours kept them: every context it
made (one per render and output size, and one for frame generation) compiled its passes again unless the driver's own
disk cache still had them, which a renamed or updated exe on NVIDIA and a driver without a disk cache do not.
`runtime/vk/vk_ffx.cpp` hands the library these creates in the place of `vkCreateComputePipelines` and
`vkCreateGraphicsPipelines`; a create that names no cache goes through ours as `pipe_create_in()` does: asked of
`P.cache` first without compiling, and compiled into the play cache when neither it nor the driver has the pipeline, so
the file a run writes (`pipeline-cache-play.bin`, or `pipeline-cache.bin` without a play cache) holds it for the next
start. Before the first pipeline of ours (`P.ready` 0) and with pipeline binaries on there is no cache to give: the
create goes to the driver as the library asked. infos: n `VkComputePipelineCreateInfo` (compute 1) or
`VkGraphicsPipelineCreateInfo`. Any thread.

### `vk_pipeline_create_foreign()`

- `if (!P.play)`: one cache: whether it held them is not known here; its save looks at the data's size.
- A copy of the create info: the flags are ours to change, the library's struct is not.
- Flags given in a `VkPipelineCreateFlags2CreateInfo` replace the struct's own, so the question "without compiling"
  cannot be put through `hd->flags`: such a create goes to the play cache at once.
- `__atomic_fetch_add(&cache_merges, 1, __ATOMIC_RELAXED)`: `autosave_once()`: the play file is due.
- `else if (r == VK_SUCCESS) r = ri`: the first that failed, as a create of several reports one result.

### `pipe_create()`

Creates one pipeline: kind 0 a graphics pipeline, 1 a graphics pipeline library, 2 a compute pipeline; ci
its `Vk*PipelineCreateInfo`. With binaries on: from the stored binaries of its pipeline key when there are any, else
compiled with `P.cache` and its binaries captured. The create info is restored before returning.

- `r = PIPE_CREATE(VK_NULL_HANDLE)`: a pipeline made from binaries takes no pipeline cache.
- `if (capture && r < 0)`: the driver refuses the capture for this kind: create without it from now on.

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
boot 17 s instead of 24. `cache_cap()`: `NX2SARIA_PIPELINE_CACHE_MAX_MB` when set (0: no limit); unset, the machine's
RAM / 32 between 1024 and 4096 MB (16 and 32 GB: 1024; 64 GB: 2048; 128 GB and more: 4096). Never below 1024: a full
cache of the pipeline list is about 550 MB on an NVIDIA card, and a limit under what the files reach compiles everything
past it again at every start, for good (RAM / 32 alone gave 509 MB on a 16 GB machine). The RAM and not the commit
headroom at the boot, so the same file is not read on one start and refused on the next. Read once, at the start; every
caller computes the same value, so the unlocked cache of it is harmless.

- `static int cache_over_cap`: the last file `cache_data_read()` was asked for is over the limit.
- `static int cache_foreign`: `pipeline-cache.bin` was written by another device or driver (a driver update).
- (A file over the limit comes from before the limit was kept on the write side, or from a lower limit.)

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

- `if (plat_file_exists(P.cache_path) || plat_file_exists(play))`: held open elsewhere: read as before, unstamped.
- The rule `cache_load()` then applies (`P.base_at_exit`): a start without `pipeline-cache.bin` writes it at its exit.
- "vk: pipeline cache stamp <id> matches|differs|none: <action>, <MB>", then what the numbers need to be checked.

### `cache_load()`

- `if (PB.on)`: the binaries carry compiled pipelines across runs; `P.cache` stays `VK_NULL_HANDLE`.
- No `pipeline-cache.bin` after the stamp's check (deleted by it, or its run ended before writing the file again): the
  save at exit writes it (`cache_save()`, `runtime/vk/vk_cache_stamp.h` `cache_base_at_exit()`).
- `int base_over = cache_over_cap`: an oversized file is never read again: the play file takes its place at once.
- The file holds this data when the driver took all of it: a save is due only once the cache has grown (`cache_save()`).
  Without data read from the file the empty cache's own size counts as saved, as the play cache's does below: a run that
  took nothing into it does not write a file of a header alone, which the next start would refuse as too short
  (`cache_file_read`) while it no longer counts as missing (`P.base_at_exit`).
- The play cache (see above `pipe_create_in()`).
- `if (base_over && cache_over_cap)`: what one run builds is over the limit: nothing will ever replace either file.
- Only a fold whose result fits: `pipeline-cache.bin` past the limit would be dropped whole at the next start
  (`cache_data_read()`). The merge into `P.cache` is the one the kept play cache gets below anyway, so it is done here
  once and its size asked; a play file mostly holds what the other already has (461.2 + 524.9 MB folded to 489.0 MB,
  2026-11-12), so the size of the two files says little. The fold is written at the first autosave, after the boot's
  prewarm and background builds have added to `P.cache`: it is taken only with room for a play file's worth of growth
  (`fold_at`) under the limit, so that save is not refused (`cache_save()` keeps the play file meanwhile).
- `fold = 0, psize = 0`: merged already: the branch for a kept play file below is skipped.
- `fold = 0`: done.
- Into `P.cache` now; the file is written by the first save, which then removes the play file. A run that ends before
  that save finds both files as they were and merges again.
- Kept as the play cache, so its file goes on holding what earlier runs made, and merged into `P.cache` in memory, where
  the first create of `pipe_create_in()` finds its pipelines. Without the merge that create took them from the driver's
  own cache when it had them, at 8 ms a pipeline against 0.4 ms from ours.
- An empty play cache is not written (its data is a header): its size counts as saved, as a whole file's does.

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

- `const char *e = cfg_get("NX2SARIA_VK_NAN_PRESERVE")`: no fixed default: it depends on the driver, see above.
- `NX2SARIA_VK_NAN_BITS`: 1 every driver, 0 none; unset: AMD's driver for RDNA 1 and 2 (`spirv_nan_bits()`).

### Stage libraries for mesh pipelines

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

- `const char *dv = cfg_get("NX2SARIA_VK_DYN_VERTEX")`: no fixed default: it depends on the GPU, see above.
- `NX2SARIA_VK_DYN_A2C=0`/1 forces dynamic alpha to coverage off or on. Default off on AMD's Windows driver branch for
  RDNA 2 and older (`vk_caps.amd_sc`), which applies alpha to coverage while the dynamic state says off: on an RX 590
  (24.9.1) NX2Saria, which never turns it on, drew Link and the NPCs with depth but no colour (black silhouettes) and
  dithered other surfaces into checkerboards (issue #67, RX 470/570/580). Taken out of the dynamic state, it goes into
  the pipeline key and the characters draw. With the texture pool marks it stays out of the dynamic state whatever the
  setting: a marked fragment shader gets early fragment tests only in a pipeline that has alpha to coverage off
  (`spirv_pool_mark()`), so the pipeline has to know.
- `const char *da = cfg_get("NX2SARIA_VK_DYN_A2C")`: no fixed default: it depends on the driver, see above.
- `pg_draws_min()`: the play gate's line at the start.

### NVN -> Vulkan mappings

### `compare_op()`

`NVN_DEPTH_FUNC_*` / `NVN_STENCIL_FUNC_*` are NEVER = 1 .. ALWAYS = 8 in `VkCompareOp` order.

### `stencil_op()`

`NVN_STENCIL_OP_*` are KEEP = 1 .. `DECR_WRAP` = 8 in `VkStencilOp` order.

### `polygon_mode()`

`NVN_POLYGON_MODE_POINT`/LINE/FILL = 0/1/2, `VkPolygonMode` FILL/LINE/POINT = 0/1/2.

### `vk_pipeline_topology()`

- `case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP`: same triangles, same coverage.
- `default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`: TRIANGLES; QUADS after the replay's index rewrite.

### `topology_class()`

Topology class (a pipeline's static topology only fixes the class): 0 point, 1 line, 2 triangle, 3 patch.

### `vk_pipeline_tex_type()`

- `default: return GFX_TEX_2D`: 2D, RECTANGLE.

### `format_type()`

Numeric type of a colour format as a shader sees it: `GFX_TYPE_*`.

### `format_features()`

Optimal-tiling (o = 1) or buffer (o = 0) features of a core format, cached.

### `widen()`

3-component 8/16-bit formats are not vertex formats on every GPU: read 4 components.

`VtxFmt`: Vertex attribute formats by NVNformat.

`vtx_formats`:

- `*_UI2F` / `*_I2F`: integer data read as float; the 8- and 16-bit ones are Vulkan's SCALED formats.
- 32-bit ones have no SCALED format: read as integers (values are not converted; logged when used).

### `target_format()`

Attachment format of an NVN format (`tex_vk_format`: the format the images are created with).

### Vertex input

Pipeline state, or `vkCmdSetVertexInputEXT`.

### `zero_format()`

An attribute the game left without a format is a fixed one on Maxwell, a single 32-bit component of 0 (an unused
attribute is bound as IsFixed 1x32 float): it reads (0, 0, 0, 1), as Vulkan expands a one-component format. The game's
foliage shaders scale wind sway by 1 - colour.w; shapes without a vertex colour (Brightbloom stem and leaves) must not
sway, or they bend away from the parts that have one (the bulb).

### `vertex_input()`

Attributes for the Locations the vertex shader reads; one without a usable NVN attribute reads `VK_PIPE_ZERO_BINDING`.

### SPIR-V rewrites

### `spirv_rebind()`

Binding decorations b >= 1 become b + off (the fragment stage's share of set 0).

### `spirv_set()`

DescriptorSet decorations become set (the fragment stage's resources move to set 1 with independent layouts).

### `spirv_dual_source()`

Dual-source blending ([maxwell.md](../maxwell.md) 5.6): SRC1 blend factors read the fragment shader's colour output 1,
which Vulkan takes from Location 0, Index 1. The Output variable at Location 1 moves there, as the translator declares
it with DualSourceBlendEnable (`tools/vendor/ryujinx/src/Ryujinx.Graphics.Shader/CodeGen/Spirv/Declarations.cs`); the
pipeline is keyed by `GKey.dsb`, so only pipelines that blend with SRC1 get this module. NULL when the module has no
Output at Location 1.

### `spirv_add_outputs()`

Adds an Output variable for each Location of `missing` (type and width from io) to a module, declared in the entry point
interface and initialised as Maxwell reads an unwritten output: (0, 0, 0, 1), the w as the bits of 1.0f whatever the
type (attribute memory is untyped); fewer than 4 components are all zero. Existing scalar, vector and pointer types are
reused. NULL when the module does not have the expected layout.

- `uint32_t bound = w[3], scalar[3] = {0, 0, 0}`: `GFX_TYPE_FLOAT`, SINT, UINT.

### `spirv_mesh_outputs()`

The locations a mesh module writes (its Output variables with a Location), and whether it draws points (execution mode
OutputPoints). 0 when the module cannot be read.

### `spirv_fs_fill_inputs()`

A fragment module whose inputs at the locations of `missing` read what Maxwell gives a fragment input no earlier stage
writes: (0, 0, 0, 1), or with `points` the point sprite's coordinate (s, t, 0, 1). Each such Input variable (a float or
a float vector) becomes a Private one, the access chains through it Private pointers, its decorations go, and the entry
function stores the value into it before anything else. NULL when nothing is to be done or the module does not have the
expected layout.

- `uint8_t *fill = calloc(old_bound, 1)`: 1: a filled variable, 2: an access chain through one.
- The variables to fill, and the Private pointer types they and the chains through them need (pass 1).
- `uint32_t vars[MAX_FILL], vtype[MAX_FILL], nvars = 0, priv[MAX_PRIV][2], npriv = 0`: priv: Input pointer type ->
  Private.
- The globals added before the first function: two constants, `gl_PointCoord`, and the Private pointer types not yet
  declared (a filled variable's goes right before the variable, below; an access chain's is only used in a function).
- `if (i == ep_at)`: the interface: filled variables out before SPIR-V 1.4 (Private is not listed), `gl_PointCoord` in.
- `i += len`: Location, Flat and the like belong to Input variables.
- `if (op == SPV_OP_VARIABLE && len >= 4 && a[1] < old_bound && fill[a[1]] == 1)`: its pointer type first.
- `if (i + len == ann_end && pc_var)`: `gl_PointCoord`'s BuiltIn after the last decoration.
- `if (op == OP_LABEL && in_entry && !stored)`: the entry's first block: its OpVariables, then the stores.
- `for (uint32_t q = 0; q < c; q++) *p++ = comp[q]`: x y z w in order: (s, t, 0, 1) or (0, 0, 0, 1).

### `spirv_fixup()`

Two fixes for what the translator declares but a shader does not use (NULL: nothing to change):

- The fragment shader interlock capability and extension, which the translator adds to every fragment shader when the
  host reports interlock support (`tools/vendor/ryujinx/src/Ryujinx.Graphics.Shader/CodeGen/Spirv/SpirvGenerator.cs`)
  and `runtime/vk/vk_device.c` does not enable; kept when the shader interlocks;
- The `CullDistance[1]` member of `gl_PerVertex` next to `ClipDistance[8]` (`Declarations.cs`
  CreatePerVertexStructType): 9 distances exceed `maxCombinedClipAndCullDistances` (8 on the RX 9070). The translator
  never accesses CullDistance; the member goes when it is the struct's last and no access chain selects it.

- `if (t < bound && t && elem[t] == cull_struct) k = 4`: arrayed per-vertex block.
- `if (cull_ok && op == OP_TYPE_STRUCT && a[0] == cull_struct) o[on] = (l - 1) << 16 | op`: last member gone.

### `uses_subgroup_ops()`

True when the module declares a `GroupNonUniform*` or SubgroupBallot-style capability (lane masks assume 32 lanes).

- `if (op != 17) return 0`: OpCapability; capabilities come first, stop at the first other instruction.
- `if ((cap >= 61 && cap <= 68) || cap == 4423 || cap == 4431) return 1`: `NonUniform*`, SubgroupBallotKHR,
  SubgroupVoteKHR.

### `spirv_reconverge()`

Guest code runs warp-wide operations (ballot, shuffle, vote) where the guest compiler had the warp reconverged, for
example right after a loop that each lane leaves at a different iteration. Vulkan only promises the same lanes at the
same point with the MaximallyReconvergesKHR execution mode. Without it a driver may run the lanes that left the loop
early through the operations on their own, with a ballot or scan that sees only some of the lanes: a mesh program that
hands out output slots from one could give two primitives the same slot. This adds the extension and the mode for each
entry point of a module that declares subgroup capabilities. NULL: nothing to change (or no memory).

- `const uint32_t ext_words = (uint32_t)(sizeof ext + 3) / 4`: the string with its terminating zero, padded.

### `spirv_mesh_invariant()`

Ocarina draws a fog sheet twice: a depth pre-pass, then a colour pass that discards where `gl_FragCoord.z` is behind the
pre-pass's depth. Both are mesh pipelines with different mesh programs, so on Maxwell the sheet's positions match bit
for bit, and the comparison relies on it. The translator marks a vertex shader's position Invariant (patch 0007), which
holds the driver to computing the same expression the same way in every pipeline; the mesh modules it makes
(`tools/xlate/MeshSpirv.cs`) declare the position without it. This adds Invariant to the Position built-in of a MeshEXT
module, as OpDecorate on a variable or OpMemberDecorate on a block member, right after its BuiltIn decoration. NULL:
nothing to change (not a mesh module, no position, Invariant already there, or no memory). Once
`tools/xlate/MeshSpirv.cs` declares it, this finds it there and changes nothing.

- `uint64_t at = 0`: the word after the position's BuiltIn decoration.
- `uint32_t target = 0, member = ~0u`: the decorated id, and its member for OpMemberDecorate.
- `for (uint64_t i = 5; i < n;)`: already Invariant.

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

- `const uint32_t ext_words = (uint32_t)(sizeof ext + 3) / 4`: the string with its terminating zero, padded.

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

- `uint32_t tu = 0, uvec[5] = {0}`: uint32 and its vectors by component count.
- Ids that are not results of the instruction's second word, inside functions: first assignment wins anyway, as a
  definition comes before its uses (OpPhi operands aside, which this never reads).
- The declarations go right before the first function: uint32 and its vectors where missing, then the constants.

### `vk_pipeline_spirv_selftest()`

`--selftest` (`runtime/main.c`): `spirv_nan_preserve()` on a small fragment module, without a device. Returns the number
of failures.

- `static const uint32_t head[] = {0x07230203, 0x00010300, 0, 5, 0, CAP, 1, CAP, 10}`: Shader, Float64.
- `enum { NH = sizeof head / 4, NB = sizeof body / 4, EP_END = 8 }`: `EP_END`: body words up to the entry point's end.
- widths: what the device allows; f64: the module declares Float64; w64: a 64-bit mode is expected.
- `if (!f64) in[NH - 1] = 0`: Matrix in place of Float64.
- `spirv_nan_bits()`: a scalar and a vec2 isnan, a select on a NaN constant; no uint32 type in the module.

### `pin_subgroup32()`

Chain {size 32} into a stage create info when the stage uses subgroup ops and the driver allows the pin.

### `local_size_x()`

The x of a compute module's LocalSize (OpExecutionMode 17), 0 when it has none.

- `if (op == 54) return 0`: OpFunction: the execution modes are all before it.

### `stage_sg()`

`StageSg`: A task or mesh stage's subgroup requirements under `NX2SARIA_VK_MESH_SUBGROUPS` (`mesh_stages_init()`), from
its SPIR-V while the caller still has it: pin, 32 lanes (the task stage; the mesh stage takes `pin_subgroup32()` as
before), and full, every subgroup of the workgroup full, where its width is a whole number of 32-lane subgroups.

### `stage_sg_apply()`

Puts g on a stage's create info; rq must live as long as it. The mesh stage's pin is already chained
(`pin_subgroup32()`).

- `if (g.full && st->pNext)`: full subgroups only with the size pinned: the requirement is then a multiple of it.

### `spirv_pool_rebind()`

Pool arrays (the translator's bindless texture and sampler arrays of `VK_POOL_TEX` / `VK_POOL_SAMP` elements, found by
their types) move to set 2, at the binding of their view kind (`runtime/vk/vk.h` `VK_POOL_KINDS()`) or `VK_POOL_KINDS`
for samplers; the kinds used are noted in `vk_pool_kinds_used` so the replay fills them. NULL: nothing to move (or no
memory).

- `int32_t *kind = malloc(bound * sizeof *kind)`: per id: image kind, 100 sampler, -1 other (types, then variables).
- Pool variables and their binding decorations are module declarations, all before the first function.
- `if (op == OP_TYPE_IMAGE && len >= 9 && w[i + 1] < bound && w[i + 7] == 1)`: sampled image.
- A translation carried over from a translator whose pool was `VK_POOL_TEX_CARRIED` long (`runtime/gfx/gfx_shader.c`,
  carry) is a pool array too: left in its own set, its textures were never bound and the scene drew with empty images
  until the background pass translated it again. It indexes the first `VK_POOL_TEX_CARRIED` entries of the larger
  binding, which Vulkan allows.
- `kind[w[i + 1]] = pool ? k + 1000 : -1`: + 1000: a pool array.
- `kind[w[i + 2]] = kind[w[i + 1]] >= 1000 ? kind[w[i + 1]] + 1000 : -1`: + 2000: a pool variable.

### Texture pool marks

`NX2SARIA_TEX_DISUSE`, default on; `runtime/vk/vk_texture.c` "disuse" uses them.

Ocarina samples nearly every texture through the bindless pool: a shader reads a handle from memory and indexes the pool
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

### `vk_pool_marks_enabled()`

- `marks_on = rt_knob("TEX_DISUSE", "1") >= 1`: 2: marks and stand-ins, nothing freed (`runtime/vk/vk_texture.c`
  "disuse").
- `vkGetPhysicalDeviceFeatures(c->phys, &f)`: `runtime/vk/vk_device.c` enables every feature the GPU reports.

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

- `uint32_t *bind = calloc(bound, sizeof *bind)`: per id: its Binding + 1 when its DescriptorSet is 2, else 0.
- `uint64_t ann_end = 0, fn_start = 0, em_end = 0`: `em_end`: past the entry points and execution modes.
- `uint32_t *rtype = calloc(bound, sizeof *rtype)`: per id: its result type, where the defer path needs it.
- Per site: 0 stored in place; else deferred, bit 1 the index is signed (a bitcast), bit 2 a ring (in a loop or outside
  the entry function).
- `if (w[i + 1] <= 3) vertex_stage = 1`: Vertex, TessellationControl, TessellationEvaluation, Geometry.
- `early = 0`: has them already.
- `early = 0`: the guest's own storage buffers: its stores would change under early tests.
- `if (op >= 19 && op <= 39 && l >= 2 && w[i + 1] < bound) is_type[w[i + 1]] = 1`: OpTypeVoid .. OpTypeForwardPointer.
- Pool texture variables: set 2, a binding below `VK_POOL_KINDS` (`spirv_pool_rebind()`); one already at the marks
  binding means the module was marked before.
- `site_def = fn_start && !marked ? calloc((n - fn_start) / 5 + 1, 1) : NULL`: an access chain is 5 words or more.
- `NX2SARIA_POOL_MARK_SHARED` (default 1): the module's ring sites share one ring and its count, and an access notes its
  slot only when it differs from the slot noted just before it (consecutive repeats take no entry). With a ring per
  site, a shader with many pool accesses in a loop (the castle moat's water: 69 shadow taps of one light's map per
  iteration) carried 69 rings of 8 dynamically indexed words and stored up to 552 marks per fragment where it ended: the
  water's draw took 7.4 ms per frame on an RTX 5080, 0.3 ms without the marks. The shared ring keeps the last RING runs
  of slots, which for such a loop is one per light, where a site's ring held its last RING accesses, all the same slot.
  It is not the union of what per-site rings held: an access order can push out a slot that a site's own ring kept. So a
  shader that noted more than RING runs sets the overflow word at its exits (`VK_POOL_MARK_OVERFLOW`), and the read that
  sees it counts every slot as sampled, for every use of the marks (`runtime/vk/vk_texture.c` `vk_pool_marks_read()`).
- `uint64_t stores = deferred + rings`: what each way out stores: a variable, or a ring's entries in a loop.
- `if (stores && (!exits || exits * (deferred + 3 * rings) > 8192))`: no way out to store at, or code that would grow.
- `memset(site_def, 0, sites), deferred = rings = stores = 0`: too much.
- From SPIR-V 1.4 on an entry point lists every global variable its functions use, not only the inputs and outputs (the
  translator's mesh shaders are 1.4): each entry point gets the marks variable, a word more.
- The mode, a word per entry point, decorations, at most 30 words of new types and the variable, 9 a site; deferred: a
  word per entry point and variable, 80 words of types and constants, then per site its variables and the store of its
  slot (ring: 10 + 29), and per way out 29 words a variable and 8 \* 29 a ring (its loop is 83).
- deferred: the types, constants and variables (pv0: one per site; ra0, rc0: a ring and its count per ring site).
- `for (uint64_t i = 5; i < ann_end;)`: the header up to the annotations' end: entry points widened, the mode added.
- `uint32_t nv = 0, *at = o + p + l + 1`: the marks buffer, then pv0.., ra0.. and rc0..
- `uint32_t k_def = 0, k_ring = 0, cur = 0`: cur: the label of the block being copied.
- Marks[the slot, or the spare when none was noted] = 1.
- more slots noted than the ring holds: the overflow word, else the spare (`VK_POOL_MARK_OVERFLOW`).
- `if (op == OP_DEMOTE_TO_HELPER)`: not the block's end: every entry, without a loop.
- The block ends here (a return, kill or terminate), so it can end in a loop over the entries noted: for (j = 0; j <
  min(count, RING); j++) mark `ring[j]`.
- `if (d)`: the slot noted for the stores at the end.
- `if ((d & 4) && shared)`: the shared ring: a slot other than the last noted goes in, count + 1.
- `} else if (d & 4)`: ring[count % RING] = slot, count + 1.
- `uint32_t at = next++`: `marks[slot]` = 1, slot being the pool index of this access.

### `spirv_mark_list_test()`

`--selftest` with `NX2SARIA_SPV_MARK_TEST`=`<list>`: each line "<in.spv> <out.spv>"; the module is moved to the pools
(`spirv_pool_rebind()`) and marked, and written to `<out>` when marked, for spirv-val. Prints the counts; 0 failures
unless a file cannot be read or written.

- Texture pool marks, below.

### `make_module()`

`early_ok`: a fragment shader whose pipeline has no alpha to coverage (`spirv_pool_mark()`).

- `const uint32_t *orig_w = w`: for the log of a module the marks cannot take.
- The hash is FNV-1a of the words `make_module()` got, as `vk_dump` hashes a shader's SPIR-V; it differs from the
  translation's only where the pipeline patched the module first (added vertex outputs).

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
