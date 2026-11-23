# runtime/nvn

The NVN front end: the game's NVN procs, implemented on host objects stored in place in the game's NVN object memory,
with command buffers recorded as op records that the back ends of `runtime/gfx/gfx.h` (`GfxBackend`) replay. The area
also holds the proc table that hands the procs out, the Tegra X1 block-linear texture layout, and two probes that
check all of it without a GPU. What the Switch's GPU and NVN do that a port must reproduce, row by row:
[maxwell.md](../maxwell.md). Where the front end sits in the host, and how submissions, held work and pacing fit
together: the bullets "NVN 2 front end" and "Frame pacing" in [architecture.md](../architecture.md); the pacer's other
half is on [pacing.md](pacing.md). The log lines: `nvn:` in [logging.md](../logging.md). The checks that drive this
code: [building.md](../building.md#checks).

In the notes below "the game" is the title the build is for, by version: 1.2.1 and 1.4.3, and Ocarina (1.1.1), whose
driver is NVN2 (Switch 2). `main+0x...` is an address in the game's main module, `sdk+0x...` one in its SDK module.

Every proc reads its arguments from the AAPCS64 registers (`arg_x`, `arg_s`, `arg_p` of `runtime/host.h`) and returns
with `ret_x` or `ret_s`. The signature of a proc is the one `runtime/nvn/nvn.h` declares as its
`PFNNVN<NAME>PROC` type; the notes give a signature only where `nvn.h` has none or where the game's use says more.

## `runtime/nvn/nvn.h`

The NVN type definitions: enums, the opaque object types with their reserved sizes, and one function-pointer type per
proc. It is another project's file, kept as it came (the MIT notice at its top; its row in `THIRD_PARTY_NOTICES.md`
says "unmodified"), so its comments stay in it and `tests/source_comments_check.py` does not read it. They say: the
types were reconstructed from another title's DWARF information and include only the enums and structs used for NVN
53.105, though they may hold more elements than expected, as that title uses a newer NVN; four opaque types carry a
note that they look not opaque and are still to be reversed.

Two sizes in it are this game's: `NVNtexture` reserves 0x80 bytes (v0 `main+214c964` allocates 128 bytes), and
`NVNtextureView` 0x10 bytes (NVN 55: 16 bytes, from the game's stack layout at `main+0x823398`).

## `runtime/nvn/nvn_obj.h`

The host-side object layouts and the command record format, shared by the interface layer (which writes them:
`runtime/nvn/nvn_api_core.c`, `runtime/nvn/nvn_api_obj.c`, `runtime/nvn/nvn_api_cmd.c`, `runtime/nvn/nvn_ext.c`) and
the replay layers (which read them: the back ends of `runtime/gfx/gfx.h`). Objects are stored in place in the NVN
object memory the game allocates, with a size not exceeding the reserved size in `runtime/nvn/nvn.h`: `FITS(T, H)` is
the static assertion. State objects (Blend, Color and the others, 4 to 0x18 bytes) are small structs the game copies by
value; their layout is packed in place with bit-fields. `MAGIC(a, b, c, d)` builds the 32-bit tags. The first line of
the file is the licence notice of the code it contains, and stays.

### Texture description and storage layout

`nvn_texdesc` and `nvn_texlayout` are the types of `runtime/nvn/nvn_tex.h`; the functions over them are in
`runtime/nvn/nvn_tex.c`.

- `nvn_texdesc`: `target`, `format`, `flags` and `samples` are NVNtextureTarget, NVNformat, NVNtextureFlags and the
  sample count (0 or 1: single-sampled). `pkg_block_h` is byte 0 of the packaged texture layout parameters + 1 (low
  nibble log2 of the block height, high nibble log2 of the block depth); 0 means the automatic algorithm.
- `nvn_texlayout`: `size` and `layer_size` are the total storage bytes and the bytes per layer; `align`, `layers` and
  `bpb` the alignment, the layer count and the bytes per block; `tileH` and `tileD` the block height and block depth
  (log2 of the GOB count).

### Objects

- `pool_t`: `game_mem`, `host` and `span` are the host-owned VM storage of physical and virtual pools; `host` is NULL
  for game-backed pools. A CPU-invisible physical or virtual pool lives in the GPU window: `mem` is its GPU address
  (never mapped on the host), `game_mem` the game's storage (never touched), `span` the reserved window bytes, `host`
  NULL. For every other pool `mem` is the game's storage, and the GPU address equals the guest address.
- `dpool_t`: a texture or sampler descriptor pool (it fits both `NVNtexturePool` and `NVNsamplerPool`).
- `tex_builder_t`: `pkg_data` is the `SetPackagedTextureData` source, copied into storage at Initialize.
- `tex_t`: `pool_flags` is the pool's flags at Initialize (in the padding): a game may finalize and free the pool
  object while the back end still looks the texture up, and the driver keeps no pointer to it either. `serial` is the
  initialization serial: the back end uses it to find its texture in the host table (the game may copy the object by
  value, so object pointers cannot be stored).
- `view_t`: the game reserves 16 bytes for `NVNtextureView` on its stack (`main+0x823398`: SetDefaults at sp+0x30, its
  own data at sp+0x40), so the view is packed into 16 bytes. `swz` holds the NVN view swizzle selectors, `dsmode` the
  NVNtextureDepthStencilMode.
- `samp_par`: the sampler parameters, one copy shared by builder and object. The enum values are all below 256.
  `lod_snap` is stored for queries; the host has no equivalent.
- `prog_t`: `serial` is the table key for the back end's resolved result.
- `win_t`: `avail[i]` is the time at which swapchain image i can be acquired again (ns, monotonic clock): its display
  time plus one present period. `pace` is the target time of this window's present pacer. `ticket[i]` is the ticket
  of the most recent present of image i.
- `cmd_t`: command records live in host memory; finished copies are owned by guest control memory.
  - `guest_flags` is at +0x10: NVN 55 inline code writes the dirty flag there.
  - `inline_cur` and `inline_end`: NVN 55 headers inline fast paths that read the command write pointer at +0x20 and
    its end at +0x28 (`main+0x70644c`) and, when 0x30 bytes fit, emit raw GPU methods themselves. ~0 / 0 never fit, so
    every call takes the API path (the `*Deferred` entry points).
  - `seg` is the start of the host recording buffer (records never go into game memory).
  - `ctl` and `ctl_size`: the control memory given by the game; recording copies live as long as it.
  - `game_size`: the command memory the game added (accounting only; not used for storage). `cmd_base`: the pool
    storage address plus the offset of the command memory the game added.
  - `rec_start` is `cmd_used` when the current recording began.
  - `hw_used`: the command memory the real NVN driver would have consumed (what GetCommandMemoryUsed reports; the game
    sizes resource heaps from it). `last_rec`: the size of the last record. `hw_start`: the usage reported to the game
    since the memory was added (`main+0x711e4c` submits only non-empty buffers).

### State objects

Their defaults follow the NVN / GL defaults. `chmask_st.mask` has 4 bits per target: r=1 g=2 b=4 a=8, a set bit
means write.

### Command records

A record is `op_hdr` {`op`, `size`} plus a payload that follows it at once; `size` includes the header and is aligned
to 8. `OP_SIGNAL_EVENT`, `OP_REPORT_COUNTER`, `OP_COPY_B2B` and `OP_UPDATE_UB` are the ops with CPU-visible side
effects.

| Payload | Note |
|---|---|
| `p_push_cc` | `OP_PUSH_CC`: NVN2 compute push constants |
| `p_viewports` | x, y, w, h, near, far (near and far are overridden by `OP_DEPTH_RANGE`) |
| `p_flag` | a single boolean render state (`OP_DEPTH_CLAMP`) |
| `p_render_cond` | SetRenderEnableConditional: mode 0 = render if the two `NVNcounterData.counter` values at `addr` and `addr`+16 are equal, 1 = if not equal |
| `p_prim_restart` | SetPrimitiveRestart: indexed strips restart at `index` |
| `p_bind_state` | `kind`: 0 blend, 1 color, 2 chmask, 3 ms, 4 poly, 5 ds, 6 point size (float), 7 line width (float) |
| `p_bind_buf` | a uniform or storage buffer |
| `p_bind_handles` | separate texture and sampler handles. `src`: NVN 55 `*Deferred` binds keep the game's handle array pointer, read at submit into `h` (`runtime/nvn/nvn_submit_guard.h` `nvn_resolve_binds()`); 0 = immediate |
| `p_stencil` | `which`: 0 value mask, 1 write mask, 2 reference value |
| `p_draw` | `instances` 0 = one instance (DrawArrays) |
| `p_draw_mesh`, `p_draw_mesh_ind` | NVN2 DrawMeshTasks(first, count) and DrawMeshTasksIndirect(indirect, x2, x3, x4) (`runtime/vk/vk_exec_draw.c` `op_draw_mesh()`). `caller`: the return address of the game's call site (to locate the dispatcher) |
| `p_copy_t2t` | reused for texture-to-buffer copies: `src` stores the destination buffer address |

## `runtime/nvn/nvn_api.h`

The internal header of the NVN front end, shared by `runtime/nvn/nvn_api_core.c` and the other slices (textures,
samplers, programs, windows, state objects, command buffers). Its licence notice stays.

### `NVN_API()`, `nvn_called()`, `NVN_LOG()`, `NVN_BLOCKING()`, `nvn_arg_bool()`

- `NVN_API(nvnX) { body }` is the strong `nvnp_nvnX(Cpu *c)`; it counts the call in `gfx_counters`, and
  `NX2SARIA_TRACE=gfx` logs the first. `nvn_called()` is its counting prologue.
- `NVN_LOG(...)` logs with the `nvn: ` prefix under `NX2SARIA_TRACE=gfx`.
- `NVN_BLOCKING()`: a wait for the GPU, the display or another thread gives the virtual core up until the end of the
  enclosing scope.
- `nvn_arg_bool()`: an NVNboolean is the low 8 bits of its register only.

### `nvn_no_object()`

1 when argument 0, the object of a getter, is NULL: the call is answered with 0 instead of reading the object at
address 0 plus a field's offset. The game's scene-loading thread makes a few such calls while an area or Kakariko's
time of day changes (a memory pool's flags, a texture builder's storage size). The first calls are logged with their
caller (`runtime/nvn/nvn_api_core.c`).

### Pools, buffers and the shared helpers

- `NVN_POOL_MAGIC` ('POOL') is `pool_t.magic` of an initialized pool. `NVN_VPOOL_PAGE` (0x10000) is the NVN virtual
  mapping granularity, 64 KiB. `nvn_gpu_only()`: the address is in the GPU window.
- `nvn_mapping_t` is NVNmappingRequest as main passes it (0x28 bytes); a NULL `physical_pool` unmaps.
- `nvn_pool_find()`: the GPU-visible pool holding [addr, addr+len], NULL if none.
- `nvn_prog_made_ns()`: `NX2SARIA_PROG_LEAD`: when the program was made, 0 when unknown.
- `nvn_new_serial()`: the next `tex_t` / `prog_t` serial, from 1.
- `nvn_call_note()`: `NX2SARIA_TRACE=perf`: a guest thread enters (`enter` 1) or leaves (0) QueueSubmitCommands
  (`present` 0) or QueuePresentTexture (1) on queue `q`: the first call of each thread, kind and queue gets a line, and
  so does a call that enters while another of these calls runs.
- `nvn_fence_last()`: the fence value of the newest submit (0 before the first).
- `nvn_depth_minus_w()`: 1 after DeviceSetDepthMode(NEAR_IS_MINUS_W), else 0.
- `nvn_submits_in_frame`: the nvnQueueSubmitCommands calls since the last present (`NX2SARIA_SUBMIT_FINISH`).
- `nvn_guest_written()`: the host or the game wrote pool storage: the back end re-reads it.
- `nvn_flush_stats()`: `NX2SARIA_TRACE=perf`: MemoryPoolFlushMappedRange [0] and BufferFlushMappedRange [1] calls [0]
  and bytes [1] by the pool's CPU class (0 CPU_CACHED, 1 CPU_UNCACHED, 2 other) since the previous call.
- `nvn_submit_prepare()`, synchronous replay: the command-buffer slice reads the deferred binds and writes the handles
  to replay to `out` (a clone where another thread's submission of the recording is in flight); the weak one is a copy
  of the handles. `nvn_submit_done()` runs after the replay of `out`; the weak one does nothing.
- `nvn_submit_prepare_async()`, asynchronous replay: as `nvn_submit_prepare()`, writing the handles to replay to `out`
  (a clone where a still queued submission uses the same recording with other deferred binds).
- `gfx_replay_async()`: 1 when the back end replays submissions on its own thread. `gfx_replayed_submits()`: the newest
  submit number (`nvn_submit_count()`) replayed. `gfx_event_signalling()`: nvnEventSignal is about to write `to` into
  the event's value.
- `NVN_HANDLE_TEX(h)` and `NVN_HANDLE_SAMP(h)`: a combined texture handle is texture id | sampler id << 20.
- `gfx_counters` is behind `gfx_stats()`; add with `__atomic_fetch_add`. Its `frames` is the back end's present;
  `nvn_calls` and `nvn_distinct` come from `NVN_API`.

### Textures, samplers, programs, windows

`NVN_TEX_MAGIC` ('TEX\0') is `tex_t.magic` of an initialized texture; `NVN_SAMP_MAGIC` ('SMP\0'), `NVN_PROG_MAGIC`
('PROG') and `NVN_WIN_MAGIC` ('WIND') are those of `samp_t`, `prog_t` and `win_t`.

- `nvn_tex_live_at()`: the live texture whose storage starts at `mem`, copied into `*out`; 0 if none. The game
  re-initializes transient textures at the same pool memory without registering them again, so a registered serial that
  was finalized since resolves here.
- `nvn_tex_view_offset()`: the byte offset of a view's first level and layer inside the texture storage
  (nvnTextureGetViewOffset); `v` may be NULL.

### Command buffers and their replay contract

A command handle (what QueueSubmitCommands receives) points at a host copy of one recording: a flat sequence of
8-byte-aligned records {`op_hdr`, payload}, `OP_BEGIN` first and `OP_END` last. Called recordings are inlined, so a copy
holds no `OP_CALL` or `OP_JUMP`. Texture pointers in payloads (`p_set_rt`, `p_copy_b2t`, `p_copy_t2t`, `p_clear_tex`)
are immutable host copies ("pins"), one per `tex_t.serial`, that live while a recording copy names them and a while
after (`nvn_pins_sweep()`). Deferred binds (`p_bind_handles.src` != 0) hold the handles read at submit
(`nvn_submit_prepare()`).

The payloads of the ops `runtime/nvn/nvn_obj.h` does not define:

| Payload | Op and note |
|---|---|
| `p_signal_event` | `OP_SIGNAL_EVENT`; `mode`: NVNeventSignalMode |
| `p_wait_event` | `OP_WAIT_EVENT`; `mode`: NVNeventWaitMode |
| `p_report_counter` | `OP_REPORT_COUNTER`: NVNcounterType, the NVNcounterData address |
| `p_reset_counter` | `OP_RESET_COUNTER` |
| `p_copy_b2b` | `OP_COPY_B2B`: GPU addresses |
| `p_update_ub` | `OP_UPDATE_UB`: `size` bytes written at `dst` |
| `p_barrier` | `OP_BARRIER`: NVNbarrierBits |
| `p_jump` | `OP_JUMP` / `OP_CALL`: the next record / the called recording |
| `p_draw_idx_ind` | `OP_DRAW_IDX_IND`: DrawElementsIndirect; `ind` = the GPU address of {count, instances, first index, base vertex, base instance} (`VkDrawIndexedIndirectCommand`'s layout), read by the GPU when the draw runs |
| `p_dispatch_ind` | `OP_DISPATCH_IND`: `ind` = the GPU address of the 3 group counts |
| `p_clear_buf` | `OP_CLEAR_BUF`: `size` bytes at `addr` filled with the 32-bit value in GPU order (ClearBuffer; ReportValue is a 4-byte one) |

- `nvn_op_first()` gives the `OP_BEGIN` record of a handle; `nvn_op_next()` the record after one.
- `nvn_handle_live()`: 1 while the handle's copy exists (its control memory was not added again).
  `nvn_handle_bytes()`: the bytes of the copy, `OP_BEGIN` to `OP_END`; 0 if not live.
- `nvn_submit_count()`: the QueueSubmitCommands calls so far.
- `NVN_PIN_AGE` (1200): the submits a texture pin outlives its last `tex_pin()` and the release of the last recording
  copy naming it: about 20 s at one submit a frame.
- `nvn_pins_sweep()` frees the texture pins no recording copy has named for `NVN_PIN_AGE` submits
  ([Texture pins](#texture-pins)), except `keep[0..nkeep)`: the replay's current render targets, which carry over to the
  next submission. It is called between submissions by the thread that replays them; `now` = `nvn_submit_count()`.
- `nvn_pins_stats()`: the pins that exist, and the pins freed so far.
- `nvn_copies_stats()`: the live recording copies and their bytes, and of them those no submission or call used for 5
  minutes.
- `nvn_op_name()`: "DRAW_IDX", ...; "?" past `OP_COUNT_`.
- `nvn_pool_invalidate()`, MemoryPoolInvalidateMappedRange: the CPU is about to read pool storage the GPU wrote. A weak
  one in `runtime/nvn/nvn_ext.c`; a back end that renders into host-side copies defines it and writes them back.
- `nvn_pool_flushed()`, MemoryPoolFlushMappedRange: the CPU wrote [mem + off, mem + off + size). A weak no-op in
  `runtime/nvn/nvn_ext.c`; a back end that keeps device-side copies of pool memory (the VRAM mirror of
  `runtime/vk/vk_memory.c`) defines it and copies the range again.

## `runtime/nvn/nvn_api_core.c`

The first slice of the front end: device, queue, memory pool, buffer, sync and event objects. Back-end calls go to
`gfx_backend()` (`runtime/gfx/gfx.h`). nvnQueuePresentTexture belongs to the window slice
(`runtime/nvn/nvn_api_obj.c`); nvnDeviceGetProcAddress lives in `runtime/nvn/nvn_procs.c`. Objects are stored in place
in the NVN object memory the game provides (`runtime/nvn/nvn_obj.h`). Its licence notice stays.

### `nvn_called()`

The call count goes to the shared counter 1024 at a time: the main thread and the job workers all record command
buffers, and one locked add per call kept the counter's cache line moving between their cores (0.8k samples a fight).
The count lags by at most 1023 calls per thread.

### `ov_memcpy()`

A `memcpy` of pool storage the GPU rendered into must copy what the GPU wrote: the game moves its generated grass maps
between pools with `memcpy`. Copies below 1 KiB skip the check; the copy itself is `memmove`, as overlapping arguments
are undefined for `memcpy` anyway.

### `gva_alloc()`, `gva_free()`, `index_add()`, `nvn_pool_find()`

The pools: the GPU window (section 3.1 of the host's design, as `runtime/gfx/gfx.h` cites it) and the address index.
`pool_lock` guards `gva` and the index. `gva` holds the ranges handed out in the GPU window, sorted by `lo`, `GVA_MAX`
(1024) at most.

`gva_alloc()` is first fit in [`RT_GPU_VA_BASE`, +`RT_GPU_VA_SIZE`); the size is a multiple of 64 KiB, so every range
stays 64 KiB aligned. `index_add()` removes the pool's entry first: a pool object may be initialized again without
Finalize.

### `pool_place()`

Physical and virtual pools. A CPU-invisible one gets a GPU window range. A CPU-visible physical pool (1.4.3: flags
0x18c, GPU_NO_ACCESS) keeps the game's storage for the CPU; the back end gives it device memory as any physical pool
(`runtime/vk/vk_memory.c` `vk_pool_init()`), which the GPU reaches through virtual pools.

### `nvnDeviceGetInteger`

`devinfo` holds the Tegra X1 values (nnheaders' `nvn_DeviceConstantsNX.h`, measured on SMO). 0x00 and 0x01,
the ABI version (55, 15), are replaced by the title's `rt_nvn_api_version` (55.15; Switch 2: 231.40). 0x5F (separate
sampler bindings, 4096) is taken from the sampler pool limit and is unverified.

NVN2 adds device properties past NVN1's 0x5F; no reference gives their values. 0x67 is read first in the game's
descriptor-pool layout (`main+0x20e8500`, with the descriptor sizes 0x0F and 0x10) and used there as an alignment
(`size & -align`): 0 made every sub-allocation 0 bytes and an assertion fail (`main+0x20e53f0`). The memory pool page
size is a safe alignment for it. The others answer 0, each logged once.

### `nvnDeviceRegisterFastClearColor`, `nvnDeviceRegisterFastClearColori`, `nvnDeviceRegisterFastClearColorui`, `nvnDeviceRegisterFastClearDepth`

Fast-clear registration is an optimisation hint; "not registered" (0) keeps the normal clear path.

### `nvnDeviceGetTextureHandle`, `nvnDeviceGetSeparateTextureHandle`, `nvnDeviceGetSeparateSamplerHandle`

The combined handle is texture id (20 bits) | sampler id << 20, the encoding the replay decodes (`NVN_HANDLE_TEX`,
`NVN_HANDLE_SAMP`). An id out of range (-1: the game had no texture to give, as when its render-target heap is full)
gets texture slot 0 and sampler slot 0: the first 256 slots of both pools are reserved (nvnDeviceGetInteger 0x11 and
0x12) and no game registers into them, so slot 0 holds the stand-in. That holds for a draw that binds the handle, where
handle 0 is "no texture" (`runtime/vk/vk_exec_draw.c`), and for a shader that takes the handle out of a uniform buffer
and indexes the pool arrays with it: slot 0 is inside their `VK_POOL_TEX` entries, which a slot past them is not. Ending
the game here turned one missing texture into a crash.

The separate handles are the descriptor index; a sampler index sits at bit 20 as in combined handles. One-word structs
return in x0.

### `nvnMemoryPoolInitialize`, `nvnMemoryPoolFinalize`, `nvnMemoryPoolMapVirtual`

Pool setup imports, mirrors and maps memory in the back end (the driver, `VirtualAlloc`): up to 20 ms on the game's
loader threads, which held a virtual core through it while job workers waited (`NX2SARIA_CORE_DIAG`). The thread gives
the core up (`NVN_BLOCKING()`), as for any blocking host call; Finalize and MapVirtual do the same.

A pool object initialized again without Finalize: the back end's record of the old pool (its import, a VRAM mirror, a
physical pool's device-local memory, its device address table record and its GPU address range) was left behind for
good, since nothing names it any more. It is finalized first (`pool_finalize()`), as nvnTextureInitialize does for a
texture; the back end lets the memory go once the GPU is past the submissions that may use it.

### `nvnMemoryPoolFlushMappedRange`, `nvnMemoryPoolMap`

The storage is coherent; a back end with device-side copies hears of a flush (`nvn_pool_flushed()`). `flush_stat` is
what `nvn_flush_stats()` reports. nvnMemoryPoolMap answers NULL for CPU_NO_ACCESS.

### `check_request()`

Every request of nvnMemoryPoolMapVirtual is checked before any mapping changes. A request without a physical pool is
an unmap.

### Buffers

Buffers reference their pool and own no storage. nvnBufferBuilderSetDefaults keeps the device. nvnBufferMap answers
NULL in a GPU-window pool, which has no CPU view. At nvnBufferFlushMappedRange the back end re-reads the range.

### `no_pool_mapping()`

The mapping of a buffer with no pool behind it (no buffer, or one whose nvnBufferInitialize failed): memory of its own,
zero at first, instead of address 0, since the game's scene-loading thread has been seen filling such a mapping (0x3d0
bytes) without looking at it. 64 KiB, or the buffer's size when it records a larger one; a larger block replaces the
smaller for later calls, and none is freed (the game may still hold one). Guest addresses are host addresses.

### Sync and events

A sync is a fence value: `fenced` = 1 after QueueFenceSync (the newest submit), 3 after WindowAcquireTexture (fence 0).

- nvnQueueFenceSync means "every command submitted to the queue so far has completed".
- nvnQueueWaitSync: the queue's later work waits for the sync on the GPU. Every queue here submits into one back-end
  timeline in submission order, so work submitted after FenceSync already runs after it: a fenced sync needs nothing.
  A sync not fenced yet would stall the queue on hardware until another queue fences it; that ordering cannot be
  expressed here and is logged once.
- nvnSyncWait: a sync never fenced reports a timeout at once; a fenced one is released only after the back end
  completed that fence value.
- nvnEventInitialize: the value lives in pool storage.
- nvnEventSignal tells the back end before the write (`gfx_event_signalling()`): the replay tells a signal that came
  after a submission's copies from one before them.

### Queue

- nvnQueueBuilderSetDefaults keeps the device and sets the device's default memory sizes.
- nvnQueueBuilderGetQueueMemorySize: command + compute + control rounded to 4 KiB (the host does not use it; the game
  only allocates it).
- nvnQueueFlush does nothing: submits are already handed to the back end.

### `nvn_call_note()`, `nvnQueueSubmitCommands`

The submission is replayed on the calling thread with its core given up (section 2.2 of the host's design, as
`runtime/host.h` cites it), or queued for the replay thread (`NX2SARIA_REPLAY_ASYNC`, `runtime/vk/vk_backend.c`); the
newest fence value is what QueueFenceSync records. With the queue the fence covers replay and GPU, so the LOD guard
waits for both.

`NX2SARIA_TRACE=perf` (`nvn_call_note()`): which threads submit and present, and whether two of these calls run at
once, which happens in play (at area changes). With the replay on the submitting threads they take turns where it
matters: replays on the submission lock (`sub_lock` of `runtime/vk/vk_sync.c`), a frame's bookkeeping at its present
between two replays (`runtime/vk/vk_frame_owed.h`), a held present put out by one thread and in order
(`runtime/pacing/vk_present_held.h`), and a second submission of a recording in flight on a clone of its own
(`runtime/nvn/nvn_submit_guard.h`). An overlap line counts; it is not a fault.

In the proc:

- `q->submits` is added atomically: two of the game's threads may submit on one queue at once.
- Asynchronous: prepare and queue run under one lock, so submit numbers follow queue order
  (`gfx_replayed_submits()`).
- Synchronous: another of the game's threads may submit the same recording meanwhile: a second submission in flight
  gets a clone of its own to resolve and replay (`nvn_submit_prepare()`), freed after (`nvn_submit_done()`).
- `nvn_submits_in_frame` is added atomically: several guest threads submit at once (the pause menu's first frame on); a
  plain `++` lost counts, and with one the frame's first submission went without its wait, or a later one waited.

The wait after a submission. The first submission of a frame (since the last present) returns once the GPU has run it.
NVN starts the GPU when the submit returns and it is through a frame's first submission (Ocarina's frame-start
compute: light grid, probes) within a few milliseconds; here the replay records first and the GPU runs later. Ocarina
rewrites data those passes and the frame's lit materials read once it has submitted its later work, with no fence: with
the GPU late, part of a frame drew with the next frame's data, distant characters wrote NaN and bloom spread it (white
dialogue frames, 3% of them in 100 s of dialogue; none with this wait, at the same 58-59 fps: the GPU is idle at that
point). Copies of the data at submit made it worse, a mix of two frames' versions.

`NX2SARIA_SUBMIT_FINISH=<mask>`: bit i waits after the i-th submission of a frame (default 0x1); a plain 1 waits after
every submission, 0 never.

Since the staged mirror uploads (`runtime/vk/vk_memory.c`) the copies of guest memory a submission reads are taken
while the replay records it, and the wait ends once the replay has submitted it (`vk_sync_recorded_wait`); the GPU's
wait remains for a submission whose GPU reads guest memory itself. That wait (`exec_wait`, `runtime/vk/vk_sync.c`
`vk_sync_exec_wait()`) ends when the GPU has run the submission's passes and the results the game reads once it goes on
are in its memory, the last frame's occlusion counts among them; the write-backs and the completion thread's other side
effects are not waited for. `NX2SARIA_WB_SPLIT=0` makes it the fence's wait, which covers those too (the notes on the
executed timeline in `runtime/vk/vk_sync.c` have what each leaves out and costs).

### `obs_release_read()`

The submit latency guard for 1.2.1's per-object uniform double buffer. NVN starts the GPU when QueueSubmitCommands
returns, within microseconds; here the replay into Vulkan takes 15-30 ms first, and the GPU reads uniform memory only
after it. The game's producer `f_70008c6460` (thread 0x103e, p56) rewrites the half of its per-object uniform blocks
that was front until the last flip, 14-23 ms after the submit that still reads it, with no fence: on hardware the GPU
has read that half by then. Here the rewrite lands inside the replay, and that frame draws other objects' matrices
(phantom rocks, flat far field).

The guard makes the producer wait, at its commit point, until the newest submit has been replayed and completed.
`lod_entered` and `lod_issued` count the submits that entered QueueSubmitCommands and those that reached the back end;
`lod_fence` holds the fence of the last `LOD_RING` (64). `NX2SARIA_LOD_GUARD=0` turns the wait off.

The producer (1.2.1 main `f_70008c6460`) releases the read lock on obj+0x538 at lr 0x70008c70c8; when [obj+0x529] ==
[obj+0x528] (x19 = obj) it goes on to rewrite the back half. The wait is off the core, at most about 400 ms. The
address is 1.2.1's: no other build has this producer there, so the hook returns at once for any other version or lr.

## `runtime/nvn/nvn_api_obj.c`

The second slice: descriptor pools, textures, samplers, programs, windows with present pacing, and the state objects.
Back-end calls go to `gfx_backend()` (`runtime/gfx/gfx.h`). Command-buffer procs are in `runtime/nvn/nvn_api_cmd.c`, the
extension procs in `runtime/nvn/nvn_ext.c`. Its licence notice stays.

### Texture builder

- nvnTextureBuilderSetDefaults keeps the device.
- nvnTextureBuilderSetSize2D: `NX2SARIA_LOG_RT_SIZES=1` (diagnosis) logs who sizes textures in the 16:9 output's
  family.
- nvnTextureBuilderSetDepthStencilMode: the back end samples the depth plane; stencil-plane sampling stops the run.
- nvnTextureBuilderSetPackagedTextureData: packaged (already GPU-laid-out) data, copied into the texture's pool
  storage at Initialize.

### `storage_class()`

Storage classes are opaque compatibility identifiers: all texture storage lives in byte-addressed pools and is decoded
per texture description, so class 1 names that convention; buffer textures use class 0. It is not a Tegra memory kind.

### `live_put()`, `live_del()`, `nvn_tex_live_at()`, `tex_release()`

The game re-initializes transient textures (G-buffer and the like) every frame at the same pool memory without
registering the descriptor again, as an NVN descriptor refers to memory and stays valid on hardware. The table `live`
maps storage address -> a copy of the live texture, so the back end can redirect a registration whose serial was
finalized. `tex_release()` releases the back end's texture and the live entry of an initialized texture object.

### `nvnTextureInitialize`

- `NX2SARIA_LOG_TEX_FLAGS` (diagnosis) logs each distinct (flags, format) pair once.
- A texture initialized again without Finalize releases the old serial.
- Packaged data: the layout's size comes from the game's descriptor and is checked against the pool it is copied into
  (the pool's host storage may span more than its size); without the check it was a heap overrun.

### `nvnTextureGetRawStorageClass`

`int nvnTextureGetRawStorageClass(const NVNtexture *t)`: no prototype in `runtime/nvn/nvn.h`. Call sites compare the
result with 1 and set a status bit when it differs; the meaning is unverified, so it answers 1.

### `texel_strides()`, `read_texels()`, `write_texels()`

Texel access: the back end decodes the view's level and layer. A row or image stride of 0 means tightly packed.
nvnTextureReadTexels and nvnTextureWriteTexels take (texture, view, region, memory); the `Strided` forms add the row
and image strides. Texel access is coherent: nvnTextureFlushTexels marks the storage as written for re-upload, and
nvnTextureInvalidateTexels does nothing.

### Samplers

- nvnSamplerBuilderSetDefaults keeps the device and sets the GL defaults.
- nvnSamplerBuilderSetLodClamp(builder, float min, float max): the floats arrive in v0 and v1.
- nvnSamplerBuilderSetLodSnap: stored for queries; no host equivalent.

### `nvn_prog_made_ns()`

`NX2SARIA_PROG_LEAD=1` (diagnostics): when each program was made, by serial (direct-mapped, `PROG_MADE_N` 16384 slots;
a serial that lost its slot reads 0). `runtime/vk/vk_exec_draw.c` `prog_lead()` compares it with the program's first
draw.

### `nvnProgramSetShaders`, `nvnProgramSetShadersExt`

nvnProgramSetShaders dumps the shaders after it set them: the files carry the program's serial.

`NVNboolean nvnProgramSetShadersExt(NVNprogram *p, int n, const NVNshaderDataExt *sd)`: no prototype in
`runtime/nvn/nvn.h`; the entries are {data, control, reserved} (0x18 bytes, reserved observed 0).

### The NVN2 program builder

Switch 2: SetDefaults, SetDevice and SetShaders on a builder, then
`nvnProgramInitializeFromBuilder(program, builder)`. No driver reference is available (the NVN2 driver is not in the
title's modules), so the signatures are taken to mirror nvnProgramSetShaders:

```
void nvnProgramBuilderSetDefaults(NVNprogramBuilder *b)
void nvnProgramBuilderSetDevice(NVNprogramBuilder *b, NVNdevice *d)
void nvnProgramBuilderSetShaders(NVNprogramBuilder *b, int n, const NVNshaderData *sd)
NVNboolean nvnProgramInitializeFromBuilder(NVNprogram *p, const NVNprogramBuilder *b)
```

The builder keeps the caller's array pointer, as NVN builders reference their inputs until the object is made.
`NVN_PROG_BUILDER_MAGIC` is 0x50524742 ("BGRP"). `NX2SARIA_NVN2_PROGRAMS=off` refuses every program (diagnosis).

### `shader_dump()`

`NX2SARIA_SHADER_DUMP=<dir>` writes each stage's control section and code there for studying the NVN2 layout, named
by the program's serial (`p<serial>_s<stage>`), the number frame dumps print as "prog N" (`runtime/vk/vk_dump.c`).
It also logs every distinct NVN2 stage number once (mesh and task programs: which numbers the title uses). The dump
stops at 20000 programs, so that the gameplay programs are in it too (mesh and task ones, and many draws, come late).

### State objects

Packed in place (`runtime/nvn/nvn_obj.h`). nvnMultisampleStateSetSampleLocations does nothing: programmable sample
positions are not replayed.

### Windows

- `nvnWindowBuilderSetNumActiveTextures` and its three companions: no prototype in `runtime/nvn/nvn.h`. The game sets
  two active images; the limit bounds what WindowAcquireTexture hands out.
- nvnWindowFinalize has nothing to release.

### `present_period()`, `pace()`, `sleep_until()`

Present pacing. The vsync thread ticks at a fixed 60 Hz (`VSYNC_HZ`) whatever the monitor does, so one game frame lasts
`interval` ticks. With the real-time frame step (`runtime/pacing/frame_rate.c`) game speed does not depend on the
interval, so a frame is shown when it is ready, at most at the frame limit (`NX2SARIA_FPS_LIMIT`: 30 to 240 per second
or the monitor's rate): holding it to the 2nd or 3rd tick would only add latency and 50 ms frames. In that case
`present_period()` is the limit alone (`frame_step_cap_ns()`). `NX2SARIA_PACE_HZ=<n>` (test runs) presents at n per
second whatever the game's interval.

`pace()`: the present slot is the previous slot plus one period. Early: wait for the slot. Late by less than a period:
present at once and keep the phase. Behind by a period or more: restart from now, no catch-up burst, except while a
movie plays (`frame_step_movie_hz`): then the slots are kept through a lateness up to `MOVIE_CATCHUP_NS` (1000000000)
and the frames after a late one go at once until the game is back on them. A movie shows one picture per game update
and its sound plays by the clock (`runtime/pacing/frame_rate.c` `ocarina_step()`), so every period dropped here was a
picture the movie fell behind for good: with a 70 ms frame every two seconds (`NX2SARIA_TEST_HITCH=120:70`) the updates
averaged 58.9 a second and the Triforce movie's picture fell 190 ms behind its sound every ten seconds, 2.2 s by the
end of its 117 s, at every frame limit from 60 to unlimited; with the slots kept it stayed within 22 ms. A stall of a
second or more (a load) restarts the slots as before. It returns the ns to wait.

`sleep_until()`: the high-resolution wait never returns early and at most `PLAT_WAIT_LATE_NS` late, so it takes the
wait up to that margin (plus 100000 ns) before the deadline and only the rest is spent yielding. The coarse sleep that
stood here left 1 to 2 ms of every paced frame to the yield loop.

### `img_fence_of()`

`img_fence`: the submit fence of each window image's newest present (`win_t` has no room: a side table for the first
four windows). `last_present_fence` is the submit fence at the newest present of any window.

### `nvnWindowAcquireTexture`

It blocks here, not in the present, until the image's previous present plus one period has passed (the game often
presents with a lock held). That is the time the next frame goes to the display, so the image is free as soon as that
time is reached.

The frame timing's waits count on the presenting thread (`runtime/diag/perf.c` `perf_end()`); a game may acquire on
another, whose wait here `perf_end_any()` adds to the next present. One line names both threads once the first present
is known.

On the Switch the image comes back only after the display has shown it, which needs the GPU to have finished the frame
drawn into it, so the CPU never runs more than the swapchain's depth ahead of the GPU. Here the pacer frees images by
the clock alone, so the acquire also waits for the GPU to finish the image's newest present. With the GPU slowed (frame
dump logging), this alone cut Ocarina's NaN frames from 56 to 23 of 61; the wait after a frame's first submission
(`runtime/nvn/nvn_api_core.c`, nvnQueueSubmitCommands) removes the rest. `NX2SARIA_ACQUIRE_WAIT=0` leaves it to the
clock; `=2` waits for the newest present of any image (one frame in flight at most). The setting is read as a number:
the on/off reader took every value but 0 for 1, so 2 could not be chosen. The back end's wait counts it as a fence wait
(`runtime/vk/vk_sync.c` `vk_sync_fence_wait()`), not pacing.

The frame's work starts here (`frame_start` of the back end). The sync the call fills means "the display no longer
reads this image". Presents are copies queued in submit order, so it is already satisfied: `fenced` = 3 with fence 0.

### `nvnQueuePresentTexture`

It queues the copy and returns at once. The display time comes from the pacer: the back end's present thread holds the
host present until then (back-edge pacing, as the Switch's display flips at vblank), so the present cadence does not
follow the replay time; one period later the image is free for the next acquire. Added latency: the slack of the frame
(period minus its work), as on the Switch.

- Before the core is given up: `movie_tick()` (`runtime/hle/ov_movie.c`) hands over decoded movie frames that are due
  and needs the core; `res_present()` (`runtime/gfx/resolution.c`): a DLSS mode change sets the game's render size
  again, between frames; `wide_present()` (`runtime/patches/widescreen.c`): Ocarina's DLSS render scale.
- `NX2SARIA_TEST_HITCH=<every>:<ms>`: a late frame every so many presents.
- A driver's low-latency sleep (`runtime/pacing/vk_lowlat.c`) or Reflex with DLSS Frame Generation
  (`runtime/vk/vk_dlssg.c`) holds the next frame to the period where the frame starts, before its pad read, so this
  pacer holds at most the game's ceiling: the present goes out as soon as it is drawn and the image is free again at
  once, or a ceiling's period after its slot. A wait here as well would be a second one, after the picture is drawn.
  Not while a movie plays: its slots are kept through a late frame (`pace()`), which only this pacer does, so the back
  end is told there is no period and the pacer keeps the movie's.
- The image's fence is the frame drawn into it (nvnWindowAcquireTexture waits for it).
- `frame_step_present()` (`runtime/pacing/frame_rate.c`): the real-time step follows the display times.

## `runtime/nvn/nvn_api_cmd.c`

The command-buffer slice: command buffers, their memory, recording as op records (`runtime/nvn/nvn_obj.h`), recording
copies behind command handles, the submit-time resolution of deferred binds (`nvn_submit_prepare()`, called by
nvnQueueSubmitCommands in `runtime/nvn/nvn_api_core.c`), and the `*Deferred` and memory-callback entry points. Its
licence notice stays.

Records are written into a growable host buffer per command buffer, never into the game's command memory: games hand
the same command memory to several command buffers recording at once (budgeted for real NVN encodings), and our records
are larger. The game's command and control memory is accounted only. EndRecording copies the finished recording into
host memory; the handle is that copy (a flat `OP_BEGIN` ... `OP_END` sequence) and lives until the game adds the same
control memory again. CallCommands and CopyCommands inline the called recording, so copies hold no `OP_CALL` or
`OP_JUMP`.

| Constant | Value | Meaning |
|---|---|---|
| `COPY_MAGIC` | 0x59504f43444d43 ("CMDCOPY") | the prefix of a live recording copy |
| `OP_RESERVE` | 24 | room always kept at the end of the recording buffer |
| `MIN_COMMAND_SIZE` | 512 | NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_COMMAND_SIZE (`devinfo` of `runtime/nvn/nvn_api_core.c`) |
| `MIN_CONTROL_SIZE` | 256 | NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_CONTROL_SIZE |
| `CTL_PER_RECORDING` | 64 | the control memory one recording is reported to use |
| `COPY_PAGE_SHIFT` | 16 | the recording copy index granularity (`release_copies()`) |

### `cb_disabled()`, `cb_set_disabled()`, `memory_event()`

NVN calls the game's memory callback when a recording needs command or control memory and none is left. Records live
in host memory here, so the callback is called only where the hardware certainly would: a recording starts without any
command memory, or ends without any control memory. It runs on the calling guest thread through `rt_call`; whatever
memory it adds is accounted. `Set{Command,Control}MemoryCallbackEnabled(false)` suppress it: `cb_off` maps a command
buffer to its disabled callback kinds | 0x100.

### `pool_take()`, `pool_put()`

1.4.3 initializes and finalizes command buffers about 2000 times a second while a scene loads and several times a
frame in play, and its two largest per-frame recordings take 1-2 MiB. Freeing the recording buffer at Finalize made
every new recording grow again from 64 KiB through up to five reallocations and copies, the last ones fresh 1-2 MiB
blocks the heap takes from the system: new commit and zero-filled pages twice a frame. Those growths were most of the
process's new commit requests, so when the system ran short for a moment (a Depths load at render scale 200 on 16 GB)
a 1 MiB growth was the allocation that failed. Finalized buffers therefore wait in a small pool (`POOL_SLOTS` 16) for
the next recording, and a failed allocation waits for memory instead of ending the game (`realloc_wait()`). A buffer
larger than `POOL_MAX_BYTES` (4 MiB: a recording that inlined a big one) goes back to the heap.

`pool_take()` gives the largest pooled buffer and its size, or NULL: the largest, so the big per-frame recordings stop
growing once the pool holds enough buffers of their size. `pool_put()` keeps a finalized buffer when the pool has room
or holds a smaller one (which is freed instead).

### `realloc_wait()`

A `realloc` that waits out a short commit shortage: Windows refuses commit beyond the current limit while it grows a
system-managed page file, so the same request succeeds a moment later, and a stall of some frames is better than ending
the game. It frees the pooled buffers first. NULL only after about 30 s without memory; the caller ends the game. It is
never called with a lock held.

### `cmd_reserve()`, `cmd_alloc()`, `rec()`

`cmd_reserve()`: the first record since Initialize takes a pooled buffer when one waits. Still failing after the wait
means the system has no commit left (the fatal report states it), or `seg` is no block of ours: a command buffer object
the game overwrote or copied shows a wrong magic or more used than its old size.

`cmd_alloc()` gives space for one record (header included); the header's op is filled by the caller. `rec()` gives a
zeroed payload of one record; `REC(o, OP, T)` types it. `RECN(...)`: array-carrying binds take the prefix fields plus n
elements; the replay reads only the first n.

### `hw_fix()`

The game sums nvnCommandBufferGetCommandMemoryUsed over recordings to size resource heaps, so usage must be what the
real NVN push-buffer encoding takes, not our record size. Known encodings (Maxwell methods, from a real recording):
DrawElements 12 words, DrawElementsBaseVertex 11, BindVertexAttribState 1+16, BindVertexStreamState 16+n,
BindVertexBuffer 6 words each. Others:

- BindVertexAttribState: the driver always writes all 16 attribute formats, 17 words (68 bytes).
- Uniform and storage buffers: one record per binding (CB_SIZE/ADDRESS 4 words + CB_BIND 1 on hardware).
- Texture, image and sampler binds: macro 7 + method 0x1a2c + CB_POS + one word per binding (16 + 4n bytes) unless the
  plain variant's hardware size is given (24 bytes for a single image, separate texture or separate sampler).
- BeginRecording takes 8 bytes back: `OP_BEGIN` and `OP_END` have no hardware equivalent.

### Texture pins

`pin_t`, `tex_pin()`, `pin_ref()`, `pin_refs()`, `op_pins()`, `nvn_pins_sweep()`.

Recordings refer to textures by pointer, but the game may finalize or reuse an NVNtexture after recording and before
the submit (NVN itself captured only GPU state). Records therefore point at an immutable host copy per texture
initialization (`tex_t.serial`). The game initializes its transient targets again every frame, about 29 new serials a
frame on 1.4.3, so copies kept for the run grew the heap by about 44 MiB per minute of play, standing still too, until
the commit limit ended the game after a few hours. A copy is freed once nothing can read it any more:

- `refs` counts the recording copies whose ops name it: live ones, released ones a queued submission still reads, and
  per-submission clones (`pin_refs()`, at `flatten_recording()` and wherever such a copy is freed or cloned);
- a recording in progress holds pins no copy counts yet, through its own `tex_pin()` calls and through ops inlined
  from a copy that was released since: a pin goes only `NVN_PIN_AGE` submits after its last `tex_pin()` and after
  `refs` fell to 0;
- the replay's current render targets carry over to later submissions and are compared by pointer
  (`runtime/vk/vk_exec.c` `set_rt()`), so a freed address reused by a new pin would read as the same target:
  `nvn_pins_sweep()` runs between submissions on the thread that replays them and keeps the pins it is given.

Instead of marking pins by walking every live recording each window, counting at copy and release walks each copy's ops
once more, when it is freed. `NX2SARIA_PIN_SWEEP=0` keeps every copy.

`pin_t`: the `tex_t` is first, as ops hold pins as `tex_t` pointers; `last_use` is `nsubmits` at the last `tex_pin()`
or the last drop of `refs`; `queued` says it is in `pin_q`. Lock order: `copy_lock`, then `pin_lock`. `pins` maps
serial -> `pin_t`; `pin_q` holds the pins with no counted copy, oldest first, in [`pin_qh`, `pin_qn`); `nsubmits` is
the QueueSubmitCommands calls so far. `pin_queue()` wants `pin_lock` held. `tex_pin()` takes `pin_lock` shared:
`nvn_pins_sweep()` frees under the exclusive lock; a new pin is queued at once, so it is freed like any other if no
copy ever names it. In the sweep a pin with a count is skipped: it is queued again when the count falls to 0.

`PIN_OPS`: the ops whose payload names pins, five of the op kinds (`OP_SET_RT`, `OP_COPY_B2T`, `OP_COPY_T2B`,
`OP_CLEAR_TEX`, `OP_COPY_T2T`). The walks over every record of a recording (`flatten_recording()`, `pin_refs()`) test
this mask before they call `op_pins()`, which writes the pins an op's payload names into `out` (at most 9) and returns
how many. `pin_refs()` counts (delta 1) or uncounts (-1) the pins named by the copy at a handle, `OP_BEGIN` to
`OP_END`, and returns how many.

### Recording copies

`copy_prefix`, `copy_age()`, `zombie_add()`, `copy_free()`, `doom()`, `doom_free()`, `zombie_sweep()`,
`copy_touch()`, `flatten_recording()`, `copy_add()`, `release_copies()`.

A handle points just past a `copy_prefix`. Copies are indexed by the 64 KiB block of their control-memory position, so
adding control memory again releases the copies recorded there without walking every live copy. 4 KiB pages cost about
500 lookups per AddControlMemory under the exclusive lock (1.4.3 field: 53 calls a frame, 2 MiB ranges, 3.7 copies per
page), and the recording threads waited on it.

`copy_prefix`: `deferred` is 0 unknown, 1 none, 2 some; `used_min` the minute of its last use (`copy_age()`); `pins`
the texture pins its ops name, counted in their `refs` (`pin_refs()`); `busy` the synchronous submissions of it
between their prepare and the end of their replay (`nvn_submit_prepare()`, `nvn_submit_done()`; atomic, see
`runtime/nvn/nvn_submit_guard.h`). It keeps its size of 56 bytes (a static assert): a session held over 200,000
copies, and `busy` shares the 8 bytes `pins` had. `copy_lock` guards the two maps, the counters and every prefix:
`copy_pages` maps page -> `copy_bucket`, `copy_live` handle -> the `copy_prefix` of a live copy.

The live copies are counted by the minute of their last use (made, called or submitted), so the report can say how
many no submission has used for `COPY_IDLE_MINS` without a walk of the copies under `copy_lock`, which the recording
threads take at every EndRecording. A copy is retired only when the game adds its control memory again
(nvnCommandBufferAddControlMemory), and a session of 2026-11-13 grew from 9,138 live copies (0.7 MiB) to 223,753
(58.3 MiB) in 7 minutes of play, about 38,000 more at each return to an area: these figures tell copies the game still
submits from ones it left behind. It is a ring of counters by minute, `COPY_AGE_MINS` (65536) long: 45 days, so it does
not wrap within a session (a copy last used that long before would share its slot with the minute now and count as
recent); only the minutes that have passed touch its pages. `copy_age()` wants `copy_lock` held, shared or exclusive,
and a live prefix.

Zombies. Asynchronous replay: copies released by the game, and per-submission clones, that a queued submission still
reads; freed once `gfx_replayed_submits()` reaches their `used_at`. Synchronous replay: copies the game released while
another of its threads' submission of them was still in flight (`busy`), freed at the next sweep after it ended.
`zombie_add()`, `zombie_sweep()` and `copy_find()` want `copy_lock` held; `copy_free()` and `doom_free()` want it not
held.

Released copies nothing reads any more are freed once `copy_lock` is dropped: uncounting a copy's pins walks its ops,
and that walk and `free()` under the exclusive lock held up every thread that was recording or submitting (the
main-loop thread waited there for 2.8% of its time). `doom()` chains a copy for that, through `made_at`, which nothing
reads after the release; `doom_free()` frees the chain, and `zombie_sweep()` returns the doom chain.

`copy_touch()` marks a live copy used by the submission `now` and returns its size from `OP_BEGIN` to `OP_END`
inclusive.

`flatten_recording()`: the recording [p, p + n), `OP_BEGIN` to `OP_END` (the recording buffer is one block: recording
emits no `OP_JUMP`), copied into one host block of its exact size. One allocation and one copy: growing the copy record
by record cost the game's main thread about 1 ms a frame in reallocations and small copies (1.4.3, field). The records
stay checked: a corrupt one stops here, not in the replay.

`release_copies()`: adding control memory again retires every recording copy made in it (the driver's handle lived
there). A copy a queued submission still reads, or that another thread's synchronous one has in flight, becomes a
zombie.

### Copies whose control memory the game reused

`ctl_fingerprint()`, `retire_visit()`, `retire_on()`, `retire_queue()`, `retire_drop()`, `retire_pass()`,
`retire_main()`.

`NX2SARIA_COPY_RETIRE=1`: a count, nothing is freed. The only event that retires a copy is AddControlMemory over its
control memory again (`release_copies()`), so a copy whose control memory the game has since used for other data stays
for good; a session of 2026-11-13 held 223,646 (56.3 MiB) by frame 39600. This counts those copies before anything
retires them. Our driver never writes control memory (`cmd_t.ctl` is accounting only), so the 16 bytes at a copy's
control position are whatever the game left there, and a fingerprint of them that no longer matches says the game wrote
there since. A reuse that writes the same bytes (zeros, often) is not seen: the counts are a lower bound.

The "nvn copy sweep" thread does every read of guest memory, at normal priority: a recording thread taking the
fingerprint at EndRecording could fault into the lazy heap (an untouched control block) and wait there. Every
`RETIRE_PERIOD_MS` (5000) it fingerprints the copies made since its last pass (a queue `copy_add()` fills, under
`retire_lock`), then visits up to `RETIRE_BATCH` of the copies it knows, from a cursor, and counts those no submission
used for `COPY_IDLE_MINS` whose bytes changed or whose memory is gone. It takes `copy_lock` shared only, `RETIRE_CHUNK`
lookups at a time. Each handle has one record: a copy that is no longer live (retired, or its handle reused: `made_at`
and `bytes` differ) leaves its record at its visit, and a new copy with a known handle replaces the older record.

- `FP_UNKNOWN` (0): not read: a lazily armed range, which a read would fetch from the GPU. `FP_GONE` (2): not mapped;
  every fingerprint of bytes is odd. `RS_NONE`, `RS_CHANGED`, `RS_GONE`: what a record counts in: nothing, changed,
  gone.
- `retire_lock` guards the queue and is never held with `copy_lock`. `retire_recs` and `retire_at` (handle -> 1 + its
  record's index) are the sweep thread's own.
- `ctl_fingerprint()`: 16 bytes of guest memory at `lo` as a fingerprint: odd, `FP_GONE` or `FP_UNKNOWN`. An untouched
  page of the lazy heap reads as zeros, so it is taken as zeros without the read that would commit it.
- `retire_visit()`: one visit (sweep thread): `live`, `used_min` and the bytes as `copy_lock` showed them; `min` is the
  minute now.
- `retire_on()`: `NX2SARIA_COPY_RETIRE`, read at the first copy.
- `retire_queue()`: called by `copy_add()` once the copy is live, `copy_lock` no longer held. A failed allocation
  leaves the copy uncounted.
- `retire_drop()` drops record i (sweep thread): it leaves the counts and the last record takes its place.
- `retire_pass()`: one pass of the sweep thread: the new copies' fingerprints, then up to `RETIRE_BATCH` visits from
  the cursor. A record that already holds a new copy's handle is of an older copy the game has retired since: a copy
  released by AddControlMemory and recorded again before the next submission can get the same block, size and
  `made_at` back, and both records would then count the one live copy. When the record array cannot grow, the rest go
  uncounted. A chunk is visited from its last record down: a removal moves the last record into the freed place, which
  then holds a record this chunk has visited or one from past it (visited at the next pass).
- `retire_main()`: the sweep runs at normal priority, as the threads it shares locks with: nearly every step of a pass
  holds one they take (`copy_lock` shared, which `copy_add()`, `release_copies()` and the submission take exclusive;
  `retire_lock`, which `retire_queue()` takes on the recording thread; the lock of `runtime/memory.c` through
  `mem_read_hold` and `lz_lock` through `mem_lazy_touches`). A holder below their priority, preempted by any thread
  above its own (the pipeline workers at BELOW_NORMAL are enough), would keep the recording and submitting threads
  waiting until the scheduler's starvation boost ran it again, seconds later. A pass is at most `RETIRE_BATCH` lookups
  and 16-byte reads every `RETIRE_PERIOD_MS`. The priority is set, not inherited from the recording thread that
  started it.

### The command buffer object and its memory

- nvnCommandBufferInitialize: the inline fast-path fields are kept at ~0 / 0 so NVN 55 inline code never finds room
  and calls the API (`cmd_t` in `runtime/nvn/nvn_obj.h`). Initialize over an object that was never finalized drops its
  recording buffer (`seg` lives in host memory). That is logged only: the old bytes may be a copy whose buffer another
  object owns, so freeing it here is not safe.
- nvnCommandBufferFinalize: handles outlive their recorder.
- nvnCommandBufferGetMemoryCallbackData returns the pointer (the type `runtime/nvn/nvn.h` gives it returns void).
- `void nvnCommandBufferSetCommandMemoryCallbackEnabled / SetControlMemoryCallbackEnabled(NVNcommandBuffer *cmd,
  NVNboolean enabled)`: NVN 55, no prototype in `runtime/nvn/nvn.h`.
- nvnCommandBufferAddCommandMemory: accounting only; command memory can be shared by independent recordings with
  distinct control memory.
- nvnCommandBufferAddControlMemory: the SDK encodes handles in control memory (`sdk+0x444530`), so reusing that storage
  retires the copies recorded there.
- nvnCommandBufferGet{Control,Command}Memory{Size,Used,Free}: usage is bounded by the memory the game gave, so the game
  never sees more used than it provided.

### Recording

- nvnCommandBufferBeginRecording starts at `seg` again: the previous recording was copied at EndRecording.
- nvnCommandBufferEndRecording: finalizing the recorder does not retire its handle; only reusing the control memory it
  was recorded in does.

### `inline_recording()`

nvnCommandBufferCallCommands and nvnCommandBufferCopyCommands. The called recording is copied into the caller at the
call position: the game recycles command memory in small steps, so a handle may be gone by the time the caller is
submitted. Deferred binds stay deferred. A copy is `OP_BEGIN`, its records and `OP_END` (`flatten_recording()` checked
them), so the records go over as one block; usage grows by the same bytes as record by record. The Switch's side of
it: [maxwell.md](../maxwell.md), row 4.8.

### Commands with CPU-visible results

nvnCommandBufferSignalEvent, ResetCounter, ReportCounter and CopyBufferToBuffer are performed by the back end in GPU
order.

- `nvnCommandBufferPushComputeConstants(NVNcommandBuffer *cmd, offset, ?, size_t size, const void *data)` (NVN2):
  constants the compute stage reads from its driver bank (c0, from byte 0x10), copied at record time. The arguments are
  as the game calls it (Ocarina `main+0x20f811c`: x1 0, x2 0, x3 0x40, x4 the data); x1 is taken as the byte offset.
  `PUSH_CC_MAX` is 1024. For diagnosis it logs the argument shapes the game uses (24 at most).
- nvnCommandBufferUpdateUniformBuffer: the data is copied at record time (the caller may change it afterwards) and
  written into the buffer at replay.

### Render state

The payloads are those of `runtime/nvn/nvn_obj.h`.

- nvnCommandBufferSetScissors: the replay keeps one scissor (index 0); other indices matter only to shaders that select
  a viewport index, so differing ones are reported ([maxwell.md](../maxwell.md), row 1.16).
- nvnCommandBufferSetRasterizerDiscard: rasterizer discard runs vertex work without rasterising; with no transform
  feedback that equals not drawing (row 5.10).
- nvnCommandBufferSetPrimitiveRestart: the game draws its terrain and cliff strips with 16-bit indices
  and restart at 0xffff; the replay applies it per indexed draw (`op_draw_idx`; row 5.2).
- nvnCommandBufferSetRenderEnableConditional: rendering is enabled when the two `NVNcounterData.counter` values at
  `addr` and `addr`+16 compare equal (mode 0) or not equal (mode 1). The game gates most of its ClearColor commands
  with it; the replay evaluates it in GPU order (row 1.4).
- nvnCommandBufferBind{Blend,Color,ChannelMask,Multisample,Polygon,DepthStencil}State: the state object's bytes are
  copied (`bind_state()`, in `p_bind_state.kind` order). nvnCommandBufferSetPointSize is recorded as bind-state kind 6
  with the float as payload, nvnCommandBufferSetLineWidth as kind 7.
- `bind_buffers()` takes (cmd, stage, first, n, ranges), `bind_buffer()` (cmd, stage, index, address, size).
- nvnCommandBufferBind{Uniform,Storage}BuffersDeferred: buffer ranges still bind at record time.
- nvnCommandBufferSetPolygonOffsetClamp: `NX2SARIA_POLY_OFFSET_LOG` logs what the game asks for, to tell a decal that
  relies on the offset from one that does not. The environment is read once: this runs for every offset the game sets.

### `bind_handles()`, `bind_handle_array()`, `bind_one_handle()`

Texture, image and separate handle binds. A deferred bind (the NVN 55 `*Deferred` entry points, the fallback of the
inline fast paths) keeps the pointer to the game's handle array: the original driver (`sdk+0x46cc00`,
BindTexturesDeferred) reads the handles when the command buffer is submitted, and the game fills its material handle
tables after recording and before the submit (`nvn_submit_prepare()`). `hw_bytes` is the hardware size of the plain
variant. `bind_handle_array()` takes (cmd, stage, first, n, handles), `bind_one_handle()` (cmd, stage, index, handle).

Combined texture handles share the texture slots with separate textures (sampler id in bits 20 and up). Images
(writable textures): handle = texture pool index; the game does not call RegisterImage, images and textures share their
registration.

`void nvnCommandBufferBind{Textures,Images,SeparateTextures,SeparateSamplers}Deferred(NVNcommandBuffer *cmd,
NVNshaderStage stage, int first, int n, const T *handles)`: NVN 55, no prototype in `runtime/nvn/nvn.h`.

### Draws and dispatches

- nvnCommandBufferDrawArraysInstanced: zero instances draw nothing.
- nvnCommandBufferDrawElementsInstanced is assumed to use the 6-parameter draw macro of the base-vertex form
  (unverified).
- nvnCommandBufferDrawElementsIndirect: the arguments at `indirect` are often GPU-written, so the replay draws with
  `vkCmdDrawIndexedIndirect` from the pool buffer.
- nvnCommandBufferDispatchCompute records the game's return address as `caller`.
  nvnCommandBufferDispatchComputeIndirect is `vkCmdDispatchIndirect`.
- `void nvnCommandBufferDrawMeshTasks(NVNcommandBuffer *cmd, int first, int count)` (NVN2): `count` task (or, without
  a task stage, mesh) workgroups of the bound program from index `first` (as the game calls it: x1 0, x2 12).
- `nvnCommandBufferDrawMeshTasksIndirect(cmd, NVNbufferAddress indirect, x2, x3, x4)` (NVN2): the workgroup count is
  read by the GPU at `indirect`. The game passes x2 1, x3 the same address, x4 0x4040; taken as one draw.
- `nvnCommandBufferSetGeometryProcessingMode(cmd, mode)` (NVN2): mode 1 before mesh draws. The replay picks the stages
  from the bound program, so nothing is recorded.

### Buffers, copies and hints

- nvnCommandBufferReportValue: the value lands when the GPU reaches this point. It is recorded as a 4-byte ClearBuffer,
  so it stays in GPU order with the ClearBuffer resets of the same counter (`main+0x7b9274` modes 1-3) and works in
  CPU-invisible pools; the CPU sees it after the fence, as a counter report.
- nvnCommandBufferSetColorReductionEnable and SetColorReductionThresholds do nothing: colour reduction is a Maxwell
  bandwidth hint for blending into compressed targets, with no effect on the result.
- nvnCommandBufferCopyTextureToBuffer is recorded as a `p_copy_b2t` with `src` = the destination address.
- nvnCommandBufferDownsample: level 0 of `src` into level 0 of `dst`, whole extents, filtered. It is recorded as a
  linear-filter texture copy: `copy_t2t` resolves a multisample source into an equal-size target and blits (a 2x2
  average at half size) between sizes ([maxwell.md](../maxwell.md), row 1.19).
- nvnCommandBufferSetCopyImageStride: the byte distance between the 2D slices of a buffer side in 3D / array buffer <->
  texture copies. Not replayed yet: the Vulkan copies keep tightly packed slices (`bufferImageHeight` 0). Still to do:
  carry it as `bufferImageHeight` = stride / row stride.

### Commands without an effect on this replay

nvnCommandBufferWaitSync and WaitEvent order work against other queues or the CPU, and every submit replays in order
on one queue; shader scratch memory (nvnCommandBufferSetShaderScratchMemory) and the tessellation patch size
(nvnCommandBufferSetPatchSize) have no host counterpart the replay needs.

- WaitSync logs its first call: a game that orders two queues with it replays here in submission order only.
- WaitEvent is recorded (`OP_WAIT_EVENT`) so that the replay can count the waits its submission order would have
  passed before the signal they wait for (`runtime/vk/vk_exec_xfer.c` `vk_exec_wait_event()`); it waits for nothing.

### `resolve_deferred()`, `nvn_submit_prepare()`, `nvn_submit_done()`, `nvn_submit_prepare_async()`

What the console reads at the submit call is read here, on the game thread: every handle must be a live recording, and
deferred binds take the handles the game's arrays hold now (the game fills its tables for the next frame as soon as the
submit returns). The result is written into the recording copy, or into a clone of it
(`runtime/nvn/nvn_submit_guard.h`). `resolve_deferred()` returns the number of deferred binds.

`nvn_submit_prepare()`, synchronous replay. Each copy counts the submissions between this prepare and their
`nvn_submit_done()` (`busy`): the first in flight resolves the copy in place; one another thread makes while that is
replayed resolves a clone of its own, unless the copy is known to hold no deferred binds. A handle the call gives twice
stays in place both times when its first occurrence does. `out`: the handles to replay. `copy_lock` is held shared,
which keeps every copy found here alive while it is cloned. In place are: the first submission in flight, a copy known
to hold no deferred binds, or the same handle again in this call after an occurrence that resolves in place
(`nvn_copy_again()`; only this branch pays for its scan). A clone has magic 0: it is not a live handle, and the game
never sees it.

`nvn_submit_done()`, after the replay of what `nvn_submit_prepare()` handed out: clones are freed, copies resolved in
place leave.

`nvn_submit_prepare_async()`, asynchronous replay. A copy a queued submission still reads keeps that submission's
deferred binds: when it holds deferred binds (or is not known to hold none), this submission gets a clone, resolved
here and freed after its replay.

### `nvn_copies_stats()`, `nvn_cmd_sizes()`

`nvn_cmd_sizes()` is `NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): the live recording copies and the
zombies waiting for their submissions, the texture pins with their sweep queue, and the recording-buffer pool. With
`NX2SARIA_COPY_RETIRE` it adds, of the copies unused for 5 minutes, those whose control memory the game has written
since and those whose control memory is no longer mapped (`retire_pass()`); a lower bound, as a rewrite with the same
bytes is not seen. In `nvn_copies_stats()` the retire counters are read without a lock of theirs, so negative readings
are taken as 0.

## `runtime/nvn/nvn_ext.c`

The NVN entry points beyond the core implementation that only report stored object state, attach debugging metadata,
convert timestamps or take hardware hints. None of them changes rendering results. The `*Deferred` binds and the
memory-callback switches live with the command buffer in `runtime/nvn/nvn_api_cmd.c`. Its licence notice stays.

### `nvn_pool_invalidate()`

The CPU is about to read what the GPU wrote into pool storage: the back end writes its host-side copies (the Vulkan
back end's images) back first. The game invalidates its 260x260 R16 grass maps and 1x1 pick targets this way. A
CPU-invisible pool has no storage to read.

### Debug labels, markers, groups, checkpoints

Meaningful only to NVN debugging tools, so every one does nothing: `nvn<Object>SetDebugLabel` for the twelve object
kinds, and the command buffer's PushDebugGroup{Static,Dynamic,}, PopDebugGroup, PopDebugGroupId,
InsertDebugMarker{Static,Dynamic,} and InsertCheckpoint{Static,Dynamic}. nvnDeviceIsExternalDebuggerAttached answers 0.
`void nvnDeviceSetAutomaticCheckpointsMask(NVNdevice *d, uint32_t mask)`: NVN 55, no prototype in
`runtime/nvn/nvn.h`.

### `ticks_to_ns()`

Timestamps: counters are written in Tegra 19.2 MHz ticks, the base of GetSystemTick (`rt_ticks`).

### Memory pools, buffers, textures, windows

- nvnBufferInvalidateMappedRange does nothing: buffers are pool storage, which the GPU writes directly.
- `get_swizzle()` serves both GetSwizzle procs: (object, NVNtextureSwizzle *r, *g, *b, *a).
- `ptrdiff_t nvnTextureBuilderGetViewOffset(const NVNtextureBuilder *b, const NVNtextureView *view)`: NVN 55.
- nvnTextureBuilderGetZCullStorageSize and nvnTextureGetZCullStorageSize answer 0: the depth-cull cache needs no
  storage.
- nvnWindowGetCrop: the default crop is the whole image.
- nvnTextureViewSetDepthStencilMode: STENCIL makes the replay sample a depth-stencil texture through a stencil-aspect
  view (`runtime/vk/vk_exec_draw.c` `get_view()`).

### Sampler queries

`border_from_int()` serves nvnSamplerBuilderSetBorderColor{i,ui}. The shared shapes of the getters: `lod_clamp()`
(object, float *lo, float *hi), `compare()` (object, NVNcompareMode *, NVNcompareFunc *), and the border getters
(object, float / int / uint32_t *rgba).

### Fixed-function state objects

Setters without a host equivalent store the value (the replay reports any non-default use); getters return what was
set.

- nvnColorStateSetAlphaTest: the replay does not apply the test (`runtime/vk/vk_pipeline.c`); the proc says once that
  the game asks for one.
- nvnBlendStateSetAdvancedMode: only "none" is implemented; overlap, premultiplied source and normalized destination
  only matter with an advanced mode, so their setters do nothing.

### Rasterisation state without a host equivalent

Non-default values are reported once (`ONCE(...)`, with the `nvn: ` prefix); the drawing proceeds without them.

- SetPointSize and SetLineWidth are recorded in `runtime/nvn/nvn_api_cmd.c` (`OP_BIND_STATE` kinds 6 and 7): the replay
  feeds the point size to the vertex shaders through the support buffer (translator patch 0008) and sets the line width
  for the draws of lines (`runtime/vk/vk_pipeline.c` `vk_pipeline_line_width()`).
- nvnCommandBufferSetAlphaRef is used only with the alpha test, which the replay reports.
- nvnCommandBufferSetDepthBounds: disabling is exact; an enabled range is drawn without the test and reported.
- `void nvnCommandBufferSetProvokingVertexMode(NVNcommandBuffer *cmd, int mode)` (NVN 55): the NVN default is the last
  vertex (GL convention); only the default is accepted ([maxwell.md](../maxwell.md), row 5.1).

### Tegra tiled-cache, ZCull and rasterisation hints

All do nothing. nvnCommandBufferSetTiledCacheAction is a tiled-cache flush / enable hint. After
nvnCommandBufferDiscardColor and DiscardDepthStencil the contents become undefined: keeping them is valid.
nvnCommandBufferSaveZCullData and RestoreZCullData: there is no depth-cull cache, so nothing to save or restore.
nvnCommandBufferOverrideSubtileSize and nvnCommandBufferSetZCullZF32CompressionEnable are NVN 55 hints with no prototype
in `runtime/nvn/nvn.h`. nvnProgramGetSubtileSize (NVN 55): its signature is not identified, so it stops the run.

## `runtime/nvn/nvn_procs.c`

The NVN proc table. Every name the game's proc loader asks for gets a synthetic host address through
`hostfn_register` before `hostfn_seal`; nvnBootstrapLoader and nvnDeviceGetProcAddress hand them out. `nvnp_<name>` is
the implementation of one proc: the weak definitions here stop with the proc's name, and
`runtime/nvn/nvn_api_core.c`, `runtime/nvn/nvn_api_obj.c`, `runtime/nvn/nvn_api_cmd.c` and `runtime/nvn/nvn_ext.c`
define the strong ones. The name list is `runtime/nvn/nvn_procs.inc`.

The strong `nvnp_nvnDeviceGetProcAddress` at the end of the file replaces the stand-in generated for the same row (a
`#define` renames the stand-in). Its signature: `PFNNVNGENERICFUNC nvnDeviceGetProcAddress(const NVNdevice *device,
const char *name)`.

### `nvn_trap()`, `nvn_proc_address()`

Unknown procs (NVN2 names the NVN1-era table lacks): an NVN2 driver has every name the game asks for, so a NULL answer
crashes the game at the first call (`main+0xc31484` called one). Each unknown name is bound on demand to one of
`TRAP_COUNT` (512) pre-registered traps (the hostfn table is sealed after boot): `trap_0` .. `trap_511`, one per pool
slot. A trap logs its name and caller on the first call and returns 0, a no-op for the setters most of these are;
`NX2SARIA_NVN_TRAP=fatal` stops instead. `nvn_proc_address()` gives one trap per distinct name, and NULL once the pool
is used up or before the traps are registered.

## `runtime/nvn/nvn_procs.inc`

The NVN proc names of 1.2.1, one `NVN_PROC(name)` row each, and nothing else. `tools/gen_nvn_procs.py` writes it: the
534 names the main module loads, the names only the movie library asks for through nvnDeviceGetProcAddress, and the
NVN2 procs this host implements. Change the generator, not the file.

## `runtime/nvn/nvn_hmap.h`

An open-addressing hash table from fixed-length byte keys to pointers (linear probing plus a backward shift on
deletion, no tombstones). The replay hot path uses it instead of a boxed dictionary: keys are not boxed, there is no
allocation, and a lookup does only one hash and `memcmp`. No locking: callers protect it with their own locks.
Ownership of values belongs to the caller (put and del return the replaced or deleted old value). Its licence notice
stays.

In `nvn_hmap`, `vals[i] == NULL` means an empty slot.

### `nvn_hm_put()`, `nvn_hm_del()`, `nvn_hm_clear()`

- `nvn_hm_put()`: `v` must not be NULL; it returns the old value for the same key (NULL if none). The load factor
  stays at 3/4 or below.
- `nvn_hm_del()`: after a deletion, later entries on the same probe chain are shifted back to fill the hole, so that a
  lookup can stop at the first empty slot. An entry whose ideal position is within (i, j] cannot move forward.
- `nvn_hm_clear()`: clears; when `rel` is non-null, it is called once for each value (releasing ownership).

## `runtime/nvn/nvn_pool_index.h`

Memory pools sorted by guest address. The world scene keeps about 3,200 pools alive, and the replay resolves several
dozen buffer addresses per draw (vertex, index, uniform and storage bindings). A linear scan of all pools for each of
them was the largest part of the ~87k instructions per draw. Lookups are a binary search on the range start plus a
backwards walk that is bounded by a running maximum of the range ends, so a pool nested inside or overlapping another
is still found. Not thread-safe: the caller holds the pool mutex for every call. Its licence notice stays.

In `pidx_ent`, `buf` and `res` are back-end data. In `pidx`, `maxhi[i]` is the largest `hi` among `e[0..i]`.

### `pidx_upper()`, `pidx_insert()`, `pidx_remove()`, `pidx_find()`, `pidx_find_mut()`, `pidx_overlaps()`

- `pidx_upper()`: the number of entries whose range starts at or below `addr` (the index of the first start above it).
- `pidx_insert()` returns 0 when memory runs out (the index is left unchanged).
- `pidx_remove()` removes the entry registered for `key`; it returns 0 when there is none.
- `pidx_find()`: a pool whose range [lo, hi] contains [addr, addr + len], or NULL (the same containment test as the
  linear scan it replaced).
- `pidx_find_mut()`: the same lookup for callers that update the entry they find (they hold the pool mutex as well).
- `pidx_overlaps()`: 1 when [lo, hi) overlaps a registered range (diagnostic: real NVN pools do not normally overlap).

### `pidx_query_invalidate()`, `pidx_find_cached()`

Exact-query memoization (`PIDX_QUERY_SLOTS` 256), protected by the same mutex as the index. The caller must invalidate
after every successful insertion or removal, including while the experiment is disabled. It stores indices, not
retained resource pointers. Overlapping ranges retain `pidx_find()`'s exact selection and zero-length semantics.

## `runtime/nvn/nvn_submit_guard.h`

The deferred binds of a recording copy at a synchronous submission, and what keeps a second submission of the same
copy, made by another guest thread while the first is replayed, from rewriting it (`runtime/nvn/nvn_api_cmd.c`
`nvn_submit_prepare()`, `nvn_submit_done()`). Header only, so that `tests/submit_clone_check.c` checks it without a
device. Its licence notice stays.

A deferred bind takes the handles the game's array holds at the submit call, and they are written into the recording
copy (`nvn_resolve_binds()`). With the replay on the submitting thread, a second thread that submitted the same copy
while the first one replayed it, or copied it for a held continuation (`runtime/vk/vk_exec.c` `cont_new()`), rewrote
the handles that replay was reading: textures and samplers of the next frame halfway through it. So each copy counts
the submissions between their prepare and the end of their replay (`busy`): the first one resolves in place, as before;
one that finds another in flight resolves a clone of its own, freed after its replay, as the asynchronous path does for
a copy a queued submission still reads. A clone taken while the first thread still resolves in place may hold a mix of
old and new handles in its binds' arrays only, and its own resolve then writes every deferred array again: it ends
whole.

### `nvn_copy_enter()`, `nvn_copy_again()`

`nvn_copy_enter()`: 1 when no other submission has the copy in flight; resolve it in place and call `nvn_copy_leave()`
after the replay. 0 when another one has, or this call holds it already (`nvn_copy_again()`); else take a clone and
call `nvn_copy_leave()` once the clone is made.

`nvn_copy_again()`, after `nvn_copy_enter()` returned 0 for `handles[i]` of one submit call: whether the same handle
came earlier in this call and resolves in place there (`out[j] == handles[j]`). Then the count that made it busy is
this thread's own: the copy is this submission's to resolve in place, and another thread that submits it meanwhile
takes a clone. So this occurrence stays in place too: its count is left after the replay like the first one's
(`nvn_submit_done()`, per occurrence), and no clone is made or logged for it. The asynchronous path keeps a repeat in
one call in place by its own rule (`runtime/nvn/nvn_api_cmd.c` `nvn_submit_prepare_async()`: `used_at != now`).

### `nvn_resolve_binds()`

`NvnBindNote` is told of a bind whose handles changed, before they are written: `b` holds the handles at record, `now`
the game's, `nth` how many changed binds came before this one (the changed count).

Deferred binds of the recording at `p` take the handles the game's arrays hold now: `OP_BIND_TEX`, `OP_BIND_IMG` and
`OP_BIND_SAMP` with `src` set and 0 < n <= 32, through `OP_JUMP` and `OP_CALL` (16 calls deep at most). `reads` and
`changed` count the binds read and those whose handles changed (atomic adds; either may be NULL); `note` may be NULL.
It returns how many deferred binds the recording has, or -1 with `*bad` at the record that stopped it: a corrupt one,
or a call past the depth limit.

## `runtime/nvn/nvn_tex.h`

The NVN texture storage layout (Tegra X1 block-linear); the code is `runtime/nvn/nvn_tex.c`. `nvn_texdesc` and
`nvn_texlayout` are defined in `runtime/nvn/nvn_obj.h`. Its licence notice stays (the licence text is in
`runtime/nvn/nvn_tex.c`).

### `nvn_fmt_bpb()`, `nvn_tex_level_tiles()`, `nvn_tex_is_linear()`, `nvn_tex_linear_stride()`

- `nvn_fmt_bpb()`: the bytes per block (0 = unknown format), and the block width and height.
- `nvn_tex_level_tiles()`: the block height and block depth actually used by level `level` (log2 of the GOB count):
  the per-level block height shrinks with that level's row count (block rows), and deswizzling uses it level by level.
- `nvn_tex_is_linear()`: LINEAR_RENDER_TARGET (0x40) uses pitch storage even without LINEAR (0x10). The game's
  CPU-loaded water height and mask textures use that flag alone.
- `nvn_tex_linear_stride()` is computed in 64 bits; a stride past 32 bits (a garbled descriptor) saturates instead of
  wrapping to a small one.

## `runtime/nvn/nvn_tex.c`

The storage size, alignment and per-level offsets of NVN textures (Tegra X1 block-linear layout). The block of
comment at its top is the licence notice and text of the code it contains, and stays. The layout rule on the Switch:
[maxwell.md](../maxwell.md), row 2.1.

The automatic tile height uses the NVN SDK's integer rounding: one GOB = 64 bytes × 8 rows = 512 bytes; the block
height (`tileH`, in GOBs) is picked by height × 1.5; levels are accumulated with block alignment. The game's packaged
textures (SetPackagedTextureLayout) carry 8 bytes of layout parameters: byte 0 = log2 of the block height (derived by
comparing 16 cases from the real game, 4 of which differ from the automatic algorithm); when present it takes
precedence.

### `fmt_of()`

`fmt_t`: the bytes per block, block width and block height (for uncompressed formats the block width and height are
1). Formats 0x41 to 0x50 are DXT / RGTC / BPTC: 4×4 blocks.

### `mip()`

Sizes are in 64 bits: a descriptor's width or height times its samples or bytes per block can pass 2^32 (a texture
garbled or not yet set up), which in 32 bits wrapped to a small size and a small allocation.

### `shape()`

`target` is NVNtextureTarget; the number of dimensions and whether it is layered are determined by the target:

| Target | Shape |
|---|---|
| 0 | 1D |
| 3 | 1D_ARRAY (the layer count is in the height) |
| 2 | 3D |
| 4, 6 | 2D_ARRAY / MS_ARRAY |
| 8 | CUBEMAP |
| 9 | CUBEMAP_ARRAY: the depth already counts layer-faces |
| others | 2D / MS / RECTANGLE |

### `minimal_layout_on()`

NVN v55 `sdk+0x4429c8` compares the default footprint against one-GOB tiles for MINIMAL_LAYOUT. The default is kept if
they are equal; otherwise tile H = D = 0 is used. Packaged layouts override this choice. The layout cannot be switched
on live resources: the startup-only knob (`NX2SARIA_MINIMAL_LAYOUT`) is a negative control for fresh sessions.

### `nvn_tex_layout()`

- Target 10 is BUFFER.
- Pitch storage: an explicit stride, otherwise 32-byte (LINEAR) or 128-byte (LINEAR_RENDER_TARGET) alignment. The
  driver counts pixel rows, also for block-compressed formats.
- NVN truncates height × 1.5 to whole GOB rows before choosing the power of two. Rounding up here changes, for
  example, 40 BC rows from H2 to H3 even when the total allocation size is unchanged, so padding becomes black texels.
- The packaged byte 0: the low nibble is the block height, the high nibble the block depth.

## `runtime/nvn/nvn_probe.c`

`--probe-nvn` of the game executable: the NVN front end and the null back end, called the way the game calls them:
through the real `nvnp_*` procs with a synthetic `Cpu` (AAPCS64 arguments in x0..x7 and v0..v7), on the guest main
thread, on the null back end whatever `NX2SARIA_GPU` says (the choice is made once, at the first object call, so
`probe_nvn()` sets it first). Its licence notice stays; the golden metadata holds no game bytes. It checks, in order:

- pool, buffer, texture, sampler, sync and event objects against the expected NVN semantics, and the texture storage
  sizes against original-SDK goldens;
- every op kind recorded through the CommandBuffer procs and parsed back with `nvn_op_first()`, `nvn_op_next()` and
  the payloads, and CallCommands inlining a recording;
- the null back end's CPU-visible side effects (event write and add, counter report, buffer copy, uniform update, in
  stream order across handles) landing in guest memory at submit, and not before;
- the proc table: every name of `runtime/nvn/nvn_procs.inc` resolves to its own synthetic address, and an unknown name
  to NULL (what its check expects; see `nvn_proc_address()` in `runtime/nvn/nvn_procs.c` for the traps);
- named fatal stubs stop with their name, each in a child process (this executable again,
  `NX2SARIA_PROBE_NVN_CHILD`).

The 40 layout goldens themselves are run by `--probe-nvn-tex` against `runtime/nvn/nvn_tex.c`; here four of them go
through the procs (`layout_goldens`: {format, width, height, levels, size}).

`fatal_procs`: the named fatal stubs checked in child processes are proc names that no task implements (the NvRm
interop of the NVX extension: nvnSyncGetNvRmSyncNVX, nvnTextureGetNvRmSurfaceNVX), so the check does not go stale when
the stub list shrinks.

### `call()`, `fv()`

Calling a proc. `fv()` sets a float argument: the low 32 bits of v<i>. The objects are in place like the game's
(`OBJ(T, n)`: each type is its NVN reserved size): `pool` is CPU-visible with game storage, `gpool` physical and
CPU_NO_ACCESS (the GPU window), `vpool` virtual.

### `probe_device_and_pools()`

- Getters asked of no object answer 0 instead of reading address 0 (`nvn_no_object()`); a buffer with no pool (a
  zeroed one, as a buffer whose initialization failed) maps to writable memory of its own, not to 0.
- Refused pools: no storage, PHYSICAL | VIRTUAL, a size that is not a 64 KiB multiple.
- A CPU-invisible physical pool lives in the GPU window, has no CPU view, and is not in the host address space.
- A virtual pool: requests are validated before any mapping changes. The three bad ones: unaligned, past the virtual
  pool, a source that is not physical.
- Finalize returns a GPU window range, so the pool can be initialized again.

### `probe_textures()`

A second texture and a third go into the other pools of the layout. Packaged texture data is copied into the pool
storage at Initialize. Views: levels and layers, and the byte offset of a level (hand-derived in
`runtime/nvn/nvn_tex_probe.c`). Depth-stencil mode: unset reads DEPTH and false; STENCIL is kept, read back and
compared. Then the descriptor pools.

### `probe_sync_events()`

The sync and event checks, then the QueueBuilder defaults and the queue.

### `record_all()`, `parse_all()`

Recording every op kind and parsing it back. The colour reduction calls are hints: nothing is recorded. A texture copy
with the LINEAR_FILTER flag is Downsample: the whole level 0 of each, filtered. The bytes parsed include `OP_END`.

### `record_target()`, `probe_pins()`

`record_target()`: a recording naming `tex3` as its render target; `*pin` is the texture pin in its SET_RT payload.

`probe_pins()`: texture pins are freed `NVN_PIN_AGE` submits after the last recording copy naming them is released,
unless the replay keeps them as its current render targets; a texture named again gets a new pin. AddControlMemory
over the recording's control memory retires its handle: the pin has no copy left.

### `probe_commands()`

CallCommands inlines the called recording at the call position. Adding the control memory again retires the recordings
made in it.

### `probe_null_effects()`

The null back end's side effects, at submit and in stream order. The layout is in the CPU-visible pool; the offsets
are bytes from `pmem` (0x100 and 0x200 the counters, 0x300 the uniform buffer, 0x400 and 0x500 the copy
destinations).

- GPU-window addresses hold no host memory: copies there have no effect and must not stop the run.
- A draw is ignored. The second handle's uniform update comes after the first handle's copy.
- The event: WRITE 0x11, ADD 5, then handle 2's WRITE 0x99.
- The uniform update lands at buffer + offset, and nothing around it moves.
- The buffer copy in stream order: handle 1's copy, then handle 2's update over its first 8 bytes.
- Counters: NVNcounterData is {counter, timestamp}; the null GPU reports 0 and a timestamp in 19.2 MHz ticks.

### `probe_proc_table()`, `probe_children()`

The proc table, with nvnDeviceGetProcAddress(device, name) called the way the loader calls it, and then the named
fatal stubs, each in a child process.

## `runtime/nvn/nvn_tex_probe.c`

`--probe-nvn-tex` of the game executable: the texture storage size, block height, alignment and mip offsets of
`runtime/nvn/nvn_tex.c`. Its licence notice stays.

The 40 size and block-height goldens are original-SDK CPU queries for uncompressed and BC3-sRGB boundary cases; no
game bytes. The old rounded-up heuristic mismatches 16 of them, including the 160x160 case. Block-linear address
checks belong to the deswizzler and are not part of this probe.

### `probe_nvn_tex()`

- `mip_offsets_rgba8_40x128` is hand-derived from the layout rules: RGBA8 40x128, 8 levels, block height 16 GOBs
  shrinking per level: 0, 24576, 32768, 34816, 35840, 36352, 36864, 37376, 37888.
- Pitch storage: LINEAR_RENDER_TARGET (0x40) aligns the stride to 128 bytes, LINEAR (0x10) to 32.
- A width past 2^30 (a garbled descriptor): its size in 64 bits, never wrapped to a small one. RGBA8 2^30 + 64 texels
  by 8 rows is one GOB row of (2^32 + 256)-byte rows: 2^26 + 4 GOBs of 512 bytes, 2^35 + 2048 bytes (in 32 bits the row
  wrapped to 256 bytes and the texture to 2048).
