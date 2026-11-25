# runtime/vk: the device, the replay and the synchronisation

The core of the Vulkan back end: the context every file of `runtime/vk/` shares (`vk.h`), the device and what it can do
(`vk_device.c`, `vk_caps.c`, `gpu_choice.h`), the back end's entry points and the frame's end (`vk_backend.c`), the
replay of the game's recorded command buffers (`vk_exec.c` and its header, the draws, binds and dispatches in
`vk_exec_draw.c`, the copies, clears and queries in `vk_exec_xfer.c`), the recorder thread that makes the replay's
Vulkan calls (`vk_rec.c`), the submissions, their timelines and the completion thread (`vk_sync.c`), the device
address table of the shaders' global memory accesses (`vk_gpa.c`), the per-draw uniform snapshots (`vk_usnap.c`), the
diagnostics (`vk_dump.c`, `vk_crumbs.c`, `vk_exec_probe.c`) and the small rule headers the tests check without a device
(`vk_sync_hw.h`, `vk_pass_hooks.h`, `vk_index_rewrite.h`, `vk_oq_hold.h`, `vk_target_frames.h`, `vk_frame_owed.h`,
`vk_evo_peek.h`).

What the Switch's GPU and NVN do that the replay must reproduce: [maxwell.md](../maxwell.md). Where the back end sits
in the host: "4. Graphics" in [architecture.md](../architecture.md). The replay's CPU cost and the threads it is
spread over: [replay-cpu.md](../replay-cpu.md) and [performance.md](../performance.md). The log lines of the area and
the `NX2SARIA_*` switches the notes name: [logging.md](../logging.md). The front end that records the command buffers
is on [nvn.md](nvn.md), the window and the present on [gfx.md](gfx.md); the other files of `runtime/vk/` are on the
pages [README.md](README.md#the-pages-of-runtimevk) lists.

In the notes below "the game" is the title the build is for; "the replay" is the code that turns a command buffer the
game recorded through NVN into Vulkan commands; "the submitting thread" is the game thread that submits a command
buffer and, with it, replays it; "the completion thread" runs the callbacks of finished submissions. `main+0x...` is an
address in the game's main module. "The translator" is the shader translator of `tools/xlate/`. A note that starts
with a line of code in a code span belongs to that line of the function or structure it is listed under.

A file whose first lines hold a licence notice keeps that notice in the code; the rows of `THIRD_PARTY_NOTICES.md` say
what each notice covers.

## `runtime/vk/vk_exec_probe.c`

nx2saria.exe --probe-vkexec: the Vulkan replay, through `gfx_vulkan_backend`. Sync part: empty submissions get
consecutive fence values; `fence_wait` and `fence_done` agree; completion callbacks run in submit order on the
completion thread; a guest-memory word a callback writes is visible once `fence_wait` returns; finish drains every
submission; a present of a cleared `runtime/vk/vk_texture.c` image (layout GENERAL) reads back exactly through the
swapchain capture. Clear part: a hand-built op stream (`SET_RT` to an RGBA8 and a D32 texture, whole-target clears, then
scissored clears) through submit reads back exactly with `read_texels`. Pass marks part (`test_marks`): the GPU's buffer
marks and the ring records name the last pass of a submission, and the device-lost report text says so.

### `note()`

Completion thread.

### `test_sync()`

- An empty recording as the front end hands it over: {`OP_BEGIN`, `OP_END`}.
- `vk_completion_add(prev + 1, note, (void *)(uintptr_t)(prev + 1))`: queued before the value exists, as
  `runtime/vk/vk_exec.c` does.
- A guest-memory word written by a completion callback is visible to the waiter.
- Finish drains everything.
- `uint64_t want = *first + (uint64_t)i + (i >= SUBMITS)`: the word submission sits between the two runs.

### `test_present()`

- `vk_texture_gpu_written(&t)`: the clear below is its content, not the storage.
- The exact colour check needs the bilinear blit: FSR's approximate reciprocals move a flat colour by 1/255.
- `b->finish()`: the queued present is submitted and, with capture on, read back.
- `uint32_t wv, black = 0xff000000u`: opaque black in both byte orders.
- `int r[4]`: the picture's rectangle; black bars around it when the window's shape differs
  (`runtime/gfx/gfx_present.c`).

### Clear part

A hand-built op stream through submit, read back with `read_texels`.

- `SC_X`: Pixel of the RGBA8 target after the stream: the scissored second clear inside the rectangle, the first
  elsewhere.

### `test_clear()`

- `static tex_t color, depth`: payload texture pointers live as long as the submission.
- `p = put(p, OP_CLEAR_COLOR, &c0, sizeof c0)`: whole target outside a pass: `vkCmdClearColorImage`.
- `p = put(p, OP_CLEAR_DS, &d0, sizeof d0)`: `vkCmdClearDepthStencilImage`.
- `p = put(p, OP_CLEAR_COLOR, &c1, sizeof c1)`: scissored: a pass and `vkCmdClearAttachments`.
- `p = put(p, OP_SCISSOR, &full, sizeof full)`: leaves the replay's scissor unbounded for later submissions.

### Draw part

The pipeline self-test, then a hand-built stream that clears an RGBA8 target, binds a census vertex + fragment pair and
draws one triangle; a census compute shader is dispatched. The census (`probe_census_dir`, `tools/shader_census.py`) is
read the way `vk_pipeline_selftest` reads it: a pair is a fragment shader and a vertex shader of the same source file.

- `vk_exec_draw_shutdown()`: `runtime/vk/vk_exec_draw.c`: the stand-in resources; the device must be idle.
- `vk_exec_xfer_shutdown()`: `runtime/vk/vk_exec_xfer.c`: the occlusion query pool; the device must be idle.

### `dr_prog()`

A program of one census shader as the game hands it to NVN: control section, code with its 0x30-byte header.

### `draw_prefix()`

- `layouts_equal`: texels compared equal between the two layouts (0: not compared).
- `ind_draw_texels`: indirect draws' texels compared, indirect dispatches run.

The draw part's stream up to the draw: target cleared, viewport, program, three RGBA32F vertices at 0x80000 of the pool,
identity uniforms at 0x90000.

### Pass marks part

(`runtime/vk/vk_crumbs.c`): one submission with a pass of 2 draws into the draw part's target, a dispatch and a pass of
3 draws into a second target. Once its fence is done, the buffer marks must name the last pass as both started and
finished, the ring records must hold what the three were, and `vk_pass_marks_format` (the text
`runtime/vk/vk_device.c` `device_fault_report()` logs) must name it. Without `VK_AMD_buffer_marker` or with
`NX2SARIA_PASS_CRUMBS=0` it is skipped: NVIDIA's checkpoints can only be read once the device is lost.

### `test_draw()`

- Vertex data at 0x80000: three vertices of `DR_ATTRS` RGBA32F attributes, attribute 0 the position. Uniform data at
  0x90000: identity matrices back to back, so that a vertex shader multiplying the position by one passes it through.
- `if (diff && vk_caps.gpl)`: the same draw through the shared set 0 layout: texel-equal.
- DrawElementsIndirect (`OP_DRAW_IDX_IND`) with the pair that drew: indices {0, 1, 2} at 0xA0000; the arguments are
  copied by the GPU (`COPY_B2B`, barrier) to 0xA0200 right before the draw, so the replay must read them on the GPU. {3,
  1, 0, 0, 0} draws the same triangle (texel-equal to the direct draw); count 0 draws nothing.
- Compute: a few census compute shaders, one dispatch each, resources unbound (zero buffers and stand-in images).
- DispatchComputeIndirect: group counts {1, 1, 1} copied by the GPU to 0xA0500 first.

### Xfer part: copies, texture clear, event and counter through submit

### `xf_tex()`

Texture in the xfer pool at `off`.

### `test_xfer()`

- Linear buffers: B2B source 0x0, target 0x1000 (4 KiB); texture source 0x2000, read-backs 0x6000 and 0xA000.
- Samples passed: a reset, no draw, a report: {0, time} through the occlusion-query path.

### `test_c0()`

c0 as `vk_c0_fill` builds it (CPU only): compute storage and uniform tables, dispatch size, unbound slots zero; the
vertex stage's point size clamp and base instance.

### `test_zero()`

A texture in a physical pool's memory that nothing wrote reads zero, as Switch pool memory starts zeroed
(`NX2SARIA_FIX_ZERO`; the driver may return zeros without it, then this is a guard).

### `test_nvnx()`

ClearBuffer / ReportValue (`OP_CLEAR_BUF`), Downsample (a linear `OP_COPY_T2T` to half size) and the stencil view
aspect. Fills: 64 bytes in a CPU-visible pool, a 4-byte report over the first word in the same submission (GPU order:
the report wins), and 64 bytes in a CPU-invisible physical pool copied out with `COPY_B2B`. Downsample: a 64x64 RGBA8
texture to 32x32, each texel the 2x2 average within 1. Stencil: views of a D24S8 texture pick the stencil aspect in
STENCIL mode and depth otherwise, and a stencil-aspect sampled view of the image is created.

- `p_copy_t2t ds`: as `nvnCommandBufferDownsample` records it.

### `test_mirror()`

VRAM mirror (`runtime/vk/vk_memory.c`; skipped in import mode): a pool given its mirror as on a first buffer use, after
which buffer copies and fills read and write the mirror. The first use copies the whole pool; CPU writes between
submissions reach the mirror through write watch; GPU writes reach guest memory by the fence, and only the bytes
written: CPU bytes beside a GPU fill keep their values.

- 1: B2B in the mirror after its first-use copy, and a fill; both written back.
- 2: the CPU rewrites the source; the next submission copies the new bytes.
- 3: CPU bytes in the page of a GPU fill, written before the submission, survive the write-back.
- 4: two small flushes at the ends of the pool copy their own pages, not what lies between them.
- A submission's copies reach the counter with the next one, and c4's own write-back adds its destination page.

### `test_empty_pool()`

A pool of zero bytes (NX2Saria makes some now and then) initialises as an empty pool: no Vulkan buffer or memory (a
buffer or import of 0 bytes is invalid usage) and no pool index entry, so a lookup at its address still finds the pool
that starts there, before and after the empty pool's Finalize.

### `probe_vkexec()`

- `vk_hdr_off()`: the present checks read 8-bit swapchain pixels.
- `vk_pipeline_shutdown()`: the pipelines and the draw part's stand-ins go before the device.

## `runtime/vk/vk_rec.c`

The recorder thread: see `runtime/vk/vk_rec.h`. One producer (the submitting thread, between `rec_begin` and `rec_end`)
and one consumer (the recorder) share a ring of 64-bit words. A packet is a header word (op, length in words) and the
call's arguments; its pointers point into the packet itself, so the recorder passes the payload to the driver as it is.
A packet never wraps: one that does not fit before the end is preceded by a pad. The producer publishes head after each
packet; the recorder makes the calls and advances tail, which the producer waits on when the ring is full and in
`rec_drain`. While a part is recorded the recorder spins on head; idle, it sleeps on an event, and a sleep that ends
with nothing new goes on without spinning first. The producer wakes a sleeping recorder once: it takes the sleeping flag
back itself, so the packets it makes while the recorder is still waking cost no further system call. head, tail and the
producer's own words sit on cache lines of their own: the producer writes head and its counters per packet while the
recorder reads head in its spin, and the recorder writes tail.

- `RING_WORDS`: 16 MiB: a part of a submission is ~1-2 MiB of packets in the town.
- `static _Alignas(64) volatile uint64_t head`: words published by the producer.
- `static _Alignas(64) volatile uint64_t tail`: words consumed by the recorder.
- `sleeping`: the recorder waits for `wake_ev`.
- `wake_ev`: auto-reset.
- `static _Alignas(64) uint64_t put_h`: producer: words written (published up to head).
- `tail_seen`: producer: tail as it last read it (`wait_space`); tail only grows.
- `rec_cb`: the part being recorded (set while the recorder is idle).
- `rec_live`: producer: `rec_begin` started the recorder for `rec_cb`.
- `rec_prio`: the submitting thread's priority, which the recorder takes.
- `rec_chk`: `NX2SARIA_REC_CHECK` (`rec_check_handle`).
- `rec_delay_ns`: `NX2SARIA_REC_DELAY_US` (`rec_main`).
- `part_h0`: `put_h` at `rec_begin`: where the packets of the part being recorded start.
- `part_fence`: the value of the part being recorded (`rec_begin`).
- `RS_`: Producer statistics, logged by `rec_stats_log`.
- `full_ns_all`: since the start, for `rec_wait_totals` (never reset).

### Consumer

### `rec_main()`

- `int slept = 0`: the last thing it did was sleep: nothing new then means still idle, so no spin before the next sleep.
- `if (t == h) plat_event_wait_ns(&wake_ev, 2000000)`: a missed wake costs at most 2 ms.
- `NX2SARIA_REC_DELAY_US` (debug): the recorder falls this far behind each batch it finds, as on a slow machine, so
  objects the replay lets go of meet packets it has not made yet (`NX2SARIA_REC_CHECK`).

### Producer

### `wake_if_sleeping()`

Wakes the recorder when it sleeps or is about to: whoever takes the flag back sets the event, once per sleep. Setting
the event for a recorder that has not reached its wait yet makes that wait return at once.

### `wait_space()`

- `if (put_h + need - tail_seen <= RING_WORDS) return`: room even by the older tail.

### `put()`

Room for a packet of bytes of arguments; the caller fills it and calls `pub()`.

- Packers reserve room for optional arrays they may leave unwritten (`BeginRendering`'s depth and stencil); with
  `NX2SARIA_REC_CHECK` those words would still hold handles from the ring's previous lap.

### `rec_begin()`

- Default off on AMD GPUs. With the recorder those drivers lost the device or hung the GPU at startup on RDNA 2, 3 and
  4, under AMD's Windows driver and under RADV (issues `#5`, `#40`, `#277` and `#283`: a black screen, or
  `VK_ERROR_DEVICE_LOST` with a breadcrumb naming a part of a replay submission, and `vk_recorder` = 0 gets past it).
  Keeping objects until their submission completed and ending a part from the recorder (`rec_end_cb`) fixed the white
  scene on an RX 6700 XT, but not this: 0.2.8 turned the recorder on for every GPU and the AMD reports came back the
  same day. `NX2SARIA_VK_RECORDER=1` turns it on for an A/B run. Up to 0.10.1 the low-end GPU mode turned it on for AMD
  GPUs without mesh shaders (GCN: RX 400/500, Vega; RDNA 1): every report above is from an RDNA 2-4 card, and those all
  have mesh shaders. An RX 580 ran with it and `NX2SARIA_REC_CHECK` (no hazards), the replay's part of the game's wait
  after a frame's first submission 6.9 ms instead of 12. The mode is gone (`runtime/cfg/cfg.c` `lowend_set()`):
  `vk_recorder` = 1 turns it on there as anywhere.
- `rec_chk = rt_knob("REC_CHECK", "0")`: debug switches, read before the recorder starts.
- `rec_cb = cb`: the recorder is idle: `rec_end` drained it.

### `rec_end_cb()`

`NX2SARIA_REC_END` (default 1): the thread that recorded the part also ends it, after the breadcrumb crumb (kind 0:
none) that `runtime/vk/vk_sync.c` writes at the end of a submission. On an RX 6700 XT (driver 26.10.1, issue `#5`) parts
the recorder recorded and the submitting thread ended broke when a submission was split
(`runtime/vk/vk_sync.c` `vk_sync_split()`): the GPU hung or the scene turned green, then white. The handoff in
`rec_drain` orders the two threads, so Vulkan allows the old split; the driver did not cope with it.
`NX2SARIA_REC_END=0` brings the old split back. The line is written only when a part was recorded by the recorder: with
the recorder off (the default on AMD) the switch changes nothing, and reports read an unconditional line as proof that
the recorder was running (`#315`).

### `rec_check_handle()`

- `chk_seen`: `[destroy][made][kind][op]`.

`NX2SARIA_REC_CHECK=1` (debug, recorder on): the packets of the part being recorded are searched word by word for an
object `runtime/vk/vk_memory.c` lets go of. `put()` zeroes each packet's room, so only words a packer wrote can match. A
hit is a hazard.

- At a deferral (destroy 0) when the stamp tl is below the part's value: `runtime/vk/vk_memory.c` `collect_locked()` may
  destroy the object once the GPU reaches tl, before the part ran and, with the recorder behind, before it made the
  call;
- At a destruction (destroy 1) always: a packet not made yet hands the driver a dead handle, a made one leaves it in a
  command buffer not yet submitted.

"Made" means before the tail the recorder published, which trails it by up to 64 packets. Hazards are logged (the
first 64) and counted by kind, packet and case for `rec_check_log`; named deferrals that wait long enough are only
counted. Reads race the producer only in words below head, which stay put until the ring wraps (16 MiB later).

- `if (p > h || h - p > RING_WORDS / 2) return`: a part this large is not searched.
- `if (!n) return`: overwritten meanwhile.

### `tail_copy()`

- `FALLBACK()`: A call the ring takes no packet for: everything queued first, then the call here.

Copies n elements of arr after the fixed part at `*at` and returns where they went (NULL for none).

### Fixed-size commands: the arguments after cb, as a struct

### Commands with arrays

A struct with the call's arguments, the arrays after it, pointers into the packet.

- `A_Xfer`: buffer or image handles.

### Extension commands through function pointers

### `rec_vertex_input()`

- `a->b = tail_copy(&at, b, sizeof *b * nb), a->v = tail_copy(&at, v, sizeof *v * na)`: `pNext`: always NULL there.

### The recorder's side

## `runtime/vk/vk_rec.h`

The recorder thread (`runtime/vk/vk_rec.c`): the Vulkan calls the replay makes into its command buffer are made by a
thread of their own. `runtime/vk/vk_sync.c` brackets each part of a submission with `rec_begin` (after
`vkBeginCommandBuffer`) and `rec_end` (before `vkEndCommandBuffer`). In between, every `vkCmd*` on that command buffer
copies its arguments into a packet of a ring, in call order, and the recorder makes the real call in that order; calls
into any other command buffer (uploads, the window, probes) are made at once. A call the ring has no packet for (a
structure with a `pNext` chain) waits for the recorder to empty the ring and is then made at once on the calling thread.
`rec_end` returns once the recorder made every call. `runtime/vk/vk_sync.c` ends a part through `rec_end_cb`: the
recorder makes the last calls into a part it recorded, `vkEndCommandBuffer` included; submitting and the command pool
stay on the submitting thread.

Every `vkCmd*` the runtime calls through `runtime/vk/vk.h` goes through the `rc_*` wrappers below (the macros at the
end); the extension commands called through function pointers have `rec_*` wrappers of their own.
`NX2SARIA_VK_RECORDER=0` makes every call at once (the same calls in the same order), `=ab` switches it with the A/B
blocks (`rt_ab`), per submission part.

A recorder thread makes the Vulkan calls: the driver's work behind the commands depends on nothing the replay decides
next.

- `rec_begin()`: the submitting thread, `sub_lock` held; fence: the part's value.
- `rec_end()`: waits for the recorder; no-op when `rec_begin` did not start it.
- `rec_end_cb()`: The end of part cb: the breadcrumb crumb (0: none, `runtime/vk/vk_crumbs.c`), then
  `vkEndCommandBuffer`, both made by the recorder when it records cb, and the wait for it (1); otherwise `rec_end` and
  the breadcrumb, and the caller ends cb (0).
- `rec_drain()`: waits until the recorder made every call queued so far.
- `rec_wait_totals()`: producer time spent on a full ring and in drains.
- `rec_stats_log()`: `NX2SARIA_TRACE=perf`: packets, bytes, drains and waits since the last call.
- `enum { REC_IMAGE, ... }`: `NX2SARIA_REC_CHECK=1` (debug, recorder on): `runtime/vk/vk_memory.c` reports each object
  it defers (destroy 0, stamped tl) or destroys (destroy 1) with the value the GPU reached (gpu); a packet of the part
  being recorded that still names it is logged as a hazard when the object can go before the part ran. `rec_check_log`:
  the counts by kind and packet, at exit.
- `rec_push_tpl()`: Extension commands called through device function pointers.
  - `VkPipelineLayout layout, uint32_t set, const void *data, uint32_t bytes)`: data: bytes read by tpl.
- `rec_bool()`: also alpha to coverage.
- `rec_shading_rate()`: size x size pixels per fragment.
- `rec_pass_mark()`: A pass mark (`runtime/vk/vk_crumbs.c` `vk_pass_mark_cmd()`: a buffer marker and a checkpoint), in
  order with the commands around it.

## `runtime/vk/vk_sync_hw.h`

The newest value each of the replay's timelines may reach, and the test a value read from one of them must pass
(`runtime/vk/vk_sync.c` "a lost device in the timelines' values"). Header only, so that `tests/sync_hw_check.c` checks
it without a device.

A timeline semaphore changes only through signal operations, and each of them carries a value the program chose: for the
replay's two timelines, the queue hand-overs of its submissions (`runtime/vk/vk_sync.c` `hold_flush_locked()`) and the
host's own signals (`runtime/vk/vk_sync.c` `vk_sync_start()`'s restart, the lost-device probe,
`runtime/vk/vk_device.c` `vk_selftest()`). A live device never reports a value above the newest of them. Each such value
is noted here before its signal can happen, so a value read above the note comes from a device that answers for work it
never ran: after a timeout reset, Windows releases every waiter of a timeline, and the reports of AMD's RX 6000 cards
(0.9.1, 0.9.2) ran the completion callbacks of submissions the GPU had not finished ("occlusion query slot ... not
available after its fence") before any call returned `VK_ERROR_DEVICE_LOST`.

- `sync_hw_raise()`: Raises the note `*hw` to v; never lowers it. Any thread.
- `sync_hw_beyond()`: 1 when raw, read from a timeline whose signals were all noted in hw (read after raw), is above
  every one of them.

## `runtime/vk/vk_pass_hooks.h`

Which pass begin runs the frame's pass hooks (`runtime/vk/vk_exec.c` `vk_exec_pass_begin()`: `vk_contact_pass_begin`,
`vk_dlaa_pass`). Header only, so that `tests/pass_hooks_check.c` checks the rule without a device.

The hooks follow the game's frame pass by pass, and what they keep is the last pass's that ran them: the light target
the upscaler takes is the one of the last scene pass before the output pass (`runtime/vk/vk_dlaa.c` `vk_dlaa_pass()`). A
pass is begun more than once while it is recorded: a barrier, a texture lookup that waits for its turn or a pass of the
replay's own ends it, and the next draw begins it again. On one thread the second begin runs the hooks in the same place
of the stream, with the same targets, and changes nothing. A parallel group's pass is recorded by several threads, and
its hooks run in the group's first chunk. With the submitting thread going on past the group (`NX2SARIA_REPLAY_PIPE`) a
second begin in that chunk came after the hooks of passes later in the stream had run, and put the group's own target
back as the light target: the upscaler then took an earlier image of the frame for one frame, a flash of the whole scene
with the interface intact (Kakariko, DLSS Performance, 3 workers, 4 groups out: 51 and 52 such frames in two runs of 100
s, none with the helpers off). So a group's hooks run once, at its first chunk's first pass.

- `pass_hooks_due()`: `in_chunk`: the calling thread records a chunk of a parallel group; idx: the chunk's place in its
  group; ran: the group's hooks ran already (its first chunk began a pass).

## `runtime/vk/vk_index_rewrite.h`

Index lists for the NVN primitives Vulkan has no topology for, made from an indexed draw's own indices
(`runtime/vk/vk_exec_draw.c` `op_draw_idx()`, `NX2SARIA_QUAD_INDEX`). Header only, so that `tests/index_rewrite_check.c`
checks it without a device.

The replay draws a quad list as a triangle list and a line loop as a line strip
(`runtime/vk/vk_pipeline.c` `vk_pipeline_topology()`). For a draw without indices it makes the indices itself
(`op_draw`); an indexed one went to the GPU with the game's indices as they are: a quad list read three indices at a
time, which joins corners of neighbouring quads into triangles across the mesh, and a line loop without its closing
line. These turn the game's indices into the list the Vulkan topology wants: two triangles (0, 1, 2) and (0, 2, 3) per
quad, as the non-indexed rewrite makes them, and the loop's first index again at its end. Indices are read at their size
(1, 2 or 4 bytes, little-endian) and written as 32-bit ones; a base vertex stays the draw's.

- `idx_quads()`: Indices of a quad list as triangles: out holds 6 * (count / 4); a trailing partial quad is left out, as
  NVN draws no incomplete primitive. Returns how many were written.
- `idx_loop()`: Indices of a line loop as a strip that comes back to its first vertex: out holds count + 1 (count >= 2).
  Returns how many were written.

## `runtime/vk/vk_oq_hold.h`

The count a samples-passed report writes when the replay could not count all of its draws
(`runtime/vk/vk_exec_xfer.c` `oq_cb()`, `NX2SARIA_OQ_HOLD`). Header only, so that `tests/oq_hold_check.c` checks it
without a device.

A draw that finds every slot of the occlusion query ring still waiting for the completion thread is left uncounted, and
the report after it says only what the other draws counted, at least 1. The game reads these counts as how much of an
object shows, and a count of 1 where an object's proxy covers thousands of samples tells it the object is all but hidden
for that frame: whatever it scales by that count (a light's glare, an object's fade) drops for one frame and comes back,
a flicker. The ring fills while the GPU or the completion thread falls behind the replay, in a hitch at a few hundred
frames a second. With the hold such a report writes the larger of what was counted and what the last complete report
into the same record said: the previous frame's answer for that object. A record with no complete report remembered
keeps the old rule.

- `OQH_N`: records remembered, direct-mapped by address; a collision forgets the older one.
- `oqh_slot()`:
  - `uint64_t h = (addr >> 4) * 0x9E3779B97F4A7C15ull`: records are 16 bytes apart at least.
- `oqh_note()`: A complete report of count into the record at addr.
- `oqh_lost()`: The count to report into addr for a report that missed draws, of which the others counted `counted`.

## `runtime/vk/vk_target_frames.h`

Target history: the presented frame (`gfx_counters.frames` + 1; 0: never) each render target subresource was last
rendered into, the one before, and in how many frames. `runtime/vk/vk_exec_draw.c` `prepare_draw_ex()` lets a draw into
a target the game does not render frame after frame (a one-shot draw: a volume or lookup table generated once) wait for
its shaders and pipeline instead of skipping it. Own header so `tests/target_frames_check.c` checks the rules without
Vulkan; `runtime/vk/vk_exec.c` is the one runtime file that includes it (the table is static). Per subresource, not per
image: a volume may be generated a few slices per frame.

Recent means within `TFR_RECENT` frames, not the frame before alone: the game's output images are a ring of three, its
probe and shadow layers are refreshed in turn every second to fourth frame, and the mips of its small effect buffers
alternate between images. Sets of `TFR_WAYS`, the oldest entry replaced: hundreds of subresources are rendered a frame,
and in a direct-mapped table of 1024 a fifth of the scene passes and most of the probe layers read as never rendered
because another subresource had taken their slot. Each misread made a draw whose pipeline was still building wait for
it, so a new area was entered one pipeline at a time.

Steady targets (`NX2SARIA_PIPE_SKIP_STEADY`, default 1; 0 = as before). Recent alone called every pass one-shot in the
first frame after the game had drawn something else for a while (a loading screen between two areas, a full-screen
menu): the scene's own targets, the probe layers and the half-resolution buffers had last been rendered hundreds of
frames before, so each draw with a new pipeline waited on the replay thread, and a mesh draw, whose pipeline has no fast
link, for its whole compile. One player's session shows it three times at area changes: frames of 954, 1015 and 684 ms,
six mesh draws waiting 52 to 660 ms each in passes whose targets had been rendered in thousands of frames. An entry
therefore also counts the frames its subresource was rendered in, for as long as the image is the texture at the same
storage (its owner, the storage GPU address the texture cache keys the image by: an image handed on to a texture
elsewhere starts again at none). The guest's texture object does not serve as the owner: the game initializes its render
target objects anew every frame at the same storage, so a count kept per object never passed one. From `TFR_STEADY`
frames on the target is one the game draws again and again, and its draws are skipped while a pipeline builds, as in any
other frame, however long ago the last one was. A target generated once never gets there; one generated again at every
area load would after `TFR_STEADY` loads, by when its pipelines are recorded and prebuilt at the start.

- `TfrTarget`: One render target of a pass: the image handle as a number, the subresource, the image's owner (0
  unknown).
- `tfr_find()`: The target's entry, or with make the entry it takes (the set's oldest, an empty one first); NULL without
  make when it has none.
- `tfr_note()`: The target is rendered in frame stamp.
  - `if (e->owner != t->owner) e->owner = t->owner, e->n = 0`: another texture has the image now: its own count.
- `tfr_before()`: The frame before stamp that last rendered the target, 0 for none.
- `tfr_first()`: No frame before stamp rendered the target.
- `tfr_steady()`: `TFR_STEADY` frames or more before stamp rendered the target under the owner it has now.
- `tfr_pass_recent()`: Whether the pass with the `n_color` colour targets c and the depth target d (NULL: none) is one
  the game renders frame after frame, so that a draw into it may be skipped in frame stamp and comes back in the next.
  any 0: every target is recent (and the pass has one). any 1: a colour target is, or the depth target is while no
  colour target is rendered for the first time: the game's item icon capture draws into a new colour target with the
  per-frame depth buffer, once, and its draws skipped there stay missing from the icon until the item is captured again.
  steady 1: a steady target counts as a recent one; `*by_steady` is set when only that made the answer 1.
  - `if (i < n_color && tfr_first(t, stamp)) color_first = 1`: one never rendered is neither recent nor steady.
  - `int r[2]`: without the steady rule, and with it.

## `runtime/vk/vk_frame_owed.h`

A frame's bookkeeping at its present (`runtime/vk/vk_backend.c` `frame_end()`: the replay's per-frame state that a
present closes), in replay order. Header only, so that `tests/frame_owed_check.c` checks it without a device.

The replay reads and writes that state as it records, under the submission lock (`runtime/vk/vk_sync.c` `sub_lock`).
With the replay on the submitting threads the game presents on one thread while another may be replaying, and running
the bookkeeping then reset the state in the middle of that replay. The presenting thread does not wait for the replay:
it marks its frame owed and runs the bookkeeping itself only when it gets the lock at once; otherwise the next thread to
take the lock (`runtime/vk/vk_sync.c` submit) runs every owed frame's bookkeeping, in frame order, before its own
replay. A frame caught up behind a later present runs as two presents with no submission between them always did.

The present thread's blit of a frame reads what that frame's bookkeeping closed
(`runtime/vk/vk_dlaa.c` `vk_dlaa_frame()`: the inputs frame generation must not get across an upscaler or frame
generation switch), so the bookkeeping must come before it, as it always did when it ran at the present. A frame left
owed may still be owed when the present thread takes it: `fo_close` runs what is owed then, under the lock, waiting for
a replay in progress as the present thread's own blit submission does a moment later. The present thread is not one of
the game's threads.

- `FrameOwed`:
  - `uint64_t owed`: the newest presented frame (atomic: the presenting thread raises it, the lock's holder reads it).
  - `uint64_t done`: the newest frame whose bookkeeping ran (the lock's holder writes it; atomic for `fo_ran`).
- `fo_waiting()`: The lock's holder: whether a frame waits for its bookkeeping (one atomic load, for every submission).
- `fo_run()`: The lock's holder: the bookkeeping of every frame presented since the last run, in frame order; returns
  how many. done moves past a frame only once its bookkeeping returned (`fo_ran` reads it without the lock).
- `fo_ran()`: Any thread: whether frame's bookkeeping ran; when it did, what it wrote is visible to the caller (the
  release of done in `fo_run`).
- `fo_present()`: The presenting thread, frame numbers rising (a smaller one than owed leaves it: that frame's
  bookkeeping ran or is owed already). `try_run(arg)` takes the lock without waiting, runs `fo_run` under it and returns
  1, or returns 0 when another thread holds it. Returns whether the bookkeeping ran here.
- `fo_close()`: The present thread, before it uses a presented frame (its blit): when the frame's bookkeeping has not
  run, `run_locked` (arg) takes the lock, waiting for it, and runs `fo_run` under it. Returns whether it had to;
  afterwards `fo_ran(frame)`.

## `runtime/vk/vk_evo_peek.h`

A copy of the first held submission's wait of each event-ordered queue (`runtime/vk/vk_exec.c` "event order"), for the
readers that do not hold the submission lock. Header only, so that `tests/evo_peek_check.c` checks it without a device.

`runtime/vk/vk_backend.c` `held_resume()` and `submit_sync()` ask `runtime/vk/vk_exec.c` `vk_exec_held_ready()` whether
held work can go on, without the submission lock. Following the queue's first continuation (`runtime/vk/vk_exec.c` Cont)
there read memory another submitting thread may free meanwhile (`runtime/vk/vk_exec.c` `evo_resume()`, `cont_free()`): a
continuation's wait lies in its private copy of the recording, and an event's address read from a reused block points
anywhere. So the holder of the lock publishes what the decision needs (whether work is held, the wait it stopped at, by
value, and when it stopped) each time a queue's first continuation or its stop changes, and the readers take a copy from
one publish (a sequence lock). A reader never waits for the writer: after `EVO_PEEK_TRIES` attempts that overlap a
publish it answers "not ready", which is what losing the race to the writer answered before.

- `EvoWait`: `p_wait_event`'s layout (`runtime/nvn/nvn_api.h`; `runtime/vk/vk_exec.c` checks the two agree): passed to
  `vk_exec_event_met` as its payload.
- `EvoPeekCopy`:
  - `uint32_t has`: a continuation is held.
  - `uint32_t has_at`: it stopped at a wait (else it has not started and goes on at once).
  - `EvoWait w`: that wait.
  - `uint64_t since`: `vk_exec_submit_seq` when it stopped.
- `EvoPeek`:
  - `uint32_t seq`: odd while a publish runs.
- `evo_peek_publish()`: The writer: one at a time (`runtime/vk/vk_exec.c`: under the submission lock).
  - `__atomic_thread_fence(__ATOMIC_RELEASE)`: a reader that sees any field below also sees the odd sequence.
- `evo_peek_read()`: Any thread: 1 with `*out` a copy of one publish, 0 when every try overlapped a publish.
  - `__atomic_thread_fence(__ATOMIC_ACQUIRE)`: the fields are read before the sequence is read again.
- `evo_peek_ready()`: The decision `runtime/vk/vk_exec.c` `cont_ready()` makes for a continuation, from a copy: it can
  go on when it has not started, its wait holds (met: `vk_exec_event_met`), or it has waited more than max submissions.

## `runtime/vk/vk_usnap.c`

Uniform blocks as of the submit call. NVN starts the GPU when `nvnQueueSubmitCommands` returns and the GPU reads the
constants soon after; here the replay records for several milliseconds, and the GPU reads a pool's bytes when it runs
the part that uses them (the VRAM mirror's sync copies run then, `runtime/vk/vk_memory.c` "VRAM mirror"). Meanwhile the
game, having handed the frame over, rewrites per-object uniform blocks on other threads with no fence: late draws read
another object's matrix (phantom rocks, a flat far field; NX2Saria 1.2.1's producer `f_70008c6460`, 14-24 ms after the
submit). So at the start of a submission's recording every uniform range it binds is copied into an arena of its own,
and the uniform bindings of the replay use the copy.

- Only ranges of imported pools (memory the CPU writes) are copied, aligned out to 256 bytes (the largest
  `minUniformBufferOffsetAlignment`) and at most 64 KiB from the bound address, overlapping ones merged.
- A range the GPU wrote in a submission that has not completed is left out: its bytes reach guest memory only with that
  submission's write-back, the mirror holds them until then.
- A GPU write within the submission (UpdateUniformBuffer, a storage buffer a shader writes, a buffer copy or fill, an
  image-to-buffer copy: `vk_usnap_drop`) drops the copies it touches from then on, so later draws read the result.
- The arena goes back to a free list once the submission's fence is done (completion thread).

The copy is taken at the submit call: where `vk_exec_record` starts when the replay runs on the submitting thread, and
by the submitting thread itself with `NX2SARIA_REPLAY_ASYNC` (`vk_usnap_take`), where the record starts up to a frame
later on the replay thread. Taken only there, it held whatever the game had put into those bytes by then: in a graveyard
the block of the shadow pass came out with another block's values in the frame its allocator moved on to the next
stretch of memory, about every 364 frames, and that frame's sunlight was all in shadow (a dark frame with the interface
intact). `NX2SARIA_UBO_SNAPSHOT=0` binds live memory as before; `=ab` alternates (`NX2SARIA_AB`).

- `Arena`:
  - `uint8_t *cpu`: with DLSS (`vk_dlaa_available`) or frame generation alone (`vk_dlaa_fg_only`), the same bytes in
    host memory for the CPU's reads (`vk_usnap_bytes`): map is often device memory through the BAR, written fast but
    read back at about a microsecond a cache line. Without DLSS nothing reads them back, and the bytes go into map in
    one copy.
- `Reg`: dropped: 0, 1 (a GPU write in flight: dropped for every lookup), or the earliest place in the stream
  (`vk_exec_stream_pos`) of a drop that ran; pos: the write's place in the stream.
- `U`:
  - `int mode`: `rt_knob`: 0 off, 1 on, 2 ab.
  - `int on`: this submission has a snapshot.
  - `Rng *col`: collected ranges.
  - `Reg *reg`: merged, sorted by lo, disjoint.
  - `Wrote *wr`: GPU writes of submissions not known to be complete.
  - `uint64_t subs, regions, bytes, copy_ns, hits, drops, inflight, refused, off_subs`: since the last summary.
  - `uint64_t chunk_drops`: so far: GPU writes recorded by replay chunks that dropped copied ranges.
- `gather_lock`: U.col, `sort_col`'s buffer and collect's neighbour cache: the submitting thread (`vk_usnap_take`) and
  the replay thread (`vk_usnap_begin`, for a submission nothing was taken for) both collect.
- `Taken`: A copy taken at the submit call for the replay thread (`vk_usnap_take`), in submission order.
  - `uint64_t first, total, refused, ns`: first, n: the submission's first handle and handle count.

### `arena_get()`

- Transfer source: DLAA copies the camera block out (`runtime/vk/vk_dlaa.c` `mv_pass()`).
- Device-local through the BAR when the device has it (the GPU reads constants on every draw), else host memory.

### `arena_done()`

Completion thread: the GPU has read it.

### `collect()`

- The vertex stage's object and bone blocks: also the game's other copy on either side, which still holds last frame's
  (`runtime/vk/vk_mvprev.c`).
- Which neighbours lie in an imported pool, per block and `vk_mem_gen`: the same blocks are bound by many draws, and two
  resolves for each binding were a quarter of the snapshot's time.
- A neighbour past its pool would make the whole merged region fail the pool check below, and the bindings in it read
  live memory again.

### `sort_col()`

Sorts the collected ranges by lo. A town frame binds about 10,000 uniform ranges and qsort took half a millisecond of
the copy's 0.96 ms on the game thread, so they go through an LSD radix sort on (lo - lowest) / ALIGN, three passes of 11
bits; qsort is left for a span that does not fit in 33 bits (2 TiB).

- `if (src != U.col) memcpy(U.col, src, (size_t)n * sizeof *src)`: three passes end in tmp.

### `wr_prune()`

Keeps the GPU writes of submissions that are not done, merged into disjoint ranges sorted by lo (the terrain rewrites
one block before every draw: hundreds of equal entries), each with the newest fence among its parts.

### `wr_hits()`

`wr_prune` ran: disjoint, sorted.

### `summary()`

- `char clears[512]`: among the GPU writes: the small buffer clears and the counter records they covered.

### `copy_some()`

`copy_regions`'s work, sliced for the replay workers (`vk_exec_par`): regions of the game's memory into the arena, then
the host copy into the device copy in `COPY_SLICE` pieces.

### `copy_regions()`

The bytes of regions `reg[0..nreg`) (total bytes in all) into a's device copy, through its host copy with DLSS. With par
(the replay thread at a submission's start), split over the replay workers: about 700 regions and 0.7 MiB in Hyrule
Field, half a millisecond of the submitting thread's time on one thread.

- `if (vk_dlaa_available() || vk_dlaa_fg_only())`: the motion pass reads the camera
  (`runtime/vk/vk_dlaa.c` `mv_pass()`).
- `if (dst != a->map)`: sequential writes into the device copy.
- `vmaFlushAllocation(vk_ctx()->vma, a->alloc, 0, total)`: a no-op on coherent memory.

### `owners_wanted()`

The player's model ownership goes with a submission only for the motion vectors the upscaler or frame generation reads
(`runtime/vk/vk_exec_draw.c` `mvobj_draw()`): without either, nothing reads it, and the walk of the player's actor and
models in guest memory at every submission is left out.

### `vk_usnap_take()`

`NX2SARIA_REPLAY_ASYNC`, the submitting thread before the submission is queued for the replay: the uniform ranges it
binds are copied now, while they hold what the game submitted. `vk_usnap_begin` takes the copy over when the replay
reaches the submission, and leaves out there what GPU writes in flight touch (the replay knows them only then).

- `for (int i = 0; i < U.ncol;)`: merged, keeping what lies wholly in one imported pool.

### `adopt()`

The copy `vk_usnap_take` made for this submission becomes the replay's. 0: none was taken.

- In submission order: an entry ahead of this submission's belongs to one that never reached the replay.
- `for (int i = 0; i < U.nreg; i++)`: a GPU write in flight: the mirror holds those bytes, not guest memory.

### `vk_usnap_begin()`

- `if (!U.mode) U.mode = rt_knob("UBO_SNAPSHOT", "1") + 1`: stored +1: 0 is "not read yet".
- `wr_prune()`: `vk_usnap_drop` goes on listing the GPU's writes with snapshots off: without this the list only grew.
- Without a producer snapshot, asynchronous replay must not read present-day actor ownership.
- Merge, keeping only what lies wholly in one imported pool and no GPU write in flight touches.
- `copy_regions(U.arena, U.reg, U.nreg, total, 1)`: the replay thread at the submission's start.

### `first_above()`

First region whose end is above a.

### `vk_usnap_find()`

- Consecutive bindings mostly name the region the last one did: that one is tried before the binary search. The regions
  are sorted and disjoint, so the region that holds addr is the one `first_above` finds.
- `static _Thread_local int hint`: per thread: the replay workers look up at once.

### `vk_usnap_drop()`

- `drop_lock`: the replay workers' shaders write storage at once.

The replay workers (`runtime/vk/vk_exec.c`) record the chunks of a group at once, and with `NX2SARIA_REPLAY_PIPE` the
submitting thread goes on past the group meanwhile. A drop holds from its place in the stream on (`vk_exec_stream_pos`):
a lookup sees the drops of the ops before it and not those of the chunks after it, in its group or later ones; the
earliest place is kept. A drop an earlier chunk has not run yet when a later chunk looks the range up is not seen, and
the later draw reads the copy instead of the earlier one's result: the chunks' drops into copied ranges are counted
(`vk_usnap`: lines) so that a scene where that can happen shows in the log. None did in Kokiri Forest, Hyrule Field or
Kakariko Village (2026-11-20): every drop into a copied range came from the submitting thread, which a group's chunks
never run beside in the stream (`tests/usnap_order_check.c` forces both orders).

### `wr_overlaps()`

A GPU write of a submission not known to be complete touches [lo, hi). A linear scan: `vk_usnap_drop` appends after
`wr_prune` sorted the list, so `wr_hits`'s binary search does not hold here; only called on rare paths.

### `cpu_written()`

The same test `vk_usnap_begin` keeps regions by.

### `vk_usnap_read()`

- `int lk = vk_exec_shared()`: a chunk's drop may grow the list.

### `host_copy()`

The host copy where there is one (DLSS), else the device copy.

### `vk_usnap_shutdown()`

The arenas back on the free list (`vk_exec_draw_shutdown`: the device is idle, every completion ran).

### `vk_usnap_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): the free uniform snapshot arenas (the free list is never
trimmed) and the GPU-write list (the replay thread's; read without a lock, a line may be one write behind).

### `vk_usnap_motion_owner()`

Ownership travels with the uniform snapshot. A missing, GPU-written, or overlapping range is not a match.

- The producer excludes overlapping identities and sorts the remaining disjoint ranges once. Most scene draws are
  unowned, so do not scan every player binding for each one.

## `runtime/vk/vk_gpa.c`

The device address table: GPU address pages to Vulkan buffer device addresses, for the global memory accesses the
translator finds no storage buffer for (translator patch 0030: GlobalToStorage TryKeepDeviceAddressAccess, and the
SPIR-V back end's GetDeviceAddressPointer, which reads this layout). Ocarina's foliage, instance and light data reach
their buffers through pointers the shaders read from other buffers at an index (a phi of constant-buffer pointers and
loaded ones, a pointer spilled to local memory and loaded again); those accesses read 0 before, which drew interiors and
shadowed areas black and dropped instanced foliage.

One storage buffer of 32-bit words, bound whole to every shader that declares it (res.json "table", `cb_slot` 0x40):

- Words 0-1: the device address of a zeroed 4 KiB buffer, which an address no page maps reads and writes instead.
- Word 2: the word index of the records (`REC_WORD`).
- Words 3-4: the device address of a 4 KiB scratch buffer, where a twin write goes when its page has no twin.
- Words 16..: L1: 65536 entries, one per 16 MiB of the 40-bit GPU address space, the index of its L2 block (0: none).
- Word 65552: L2 blocks of 4096 entries, one per 4 KiB page: the record of the buffer that holds the page (0: none).
- `REC_WORD`: records of four words: the buffer's device address minus the GPU address of its byte 0, then the same for
  the page's twin (low words first).

The twin: a pool with a VRAM mirror (`runtime/vk/vk_memory.c` `mirror_create()`) has the mirror in its record's first
address and its imported buffer, guest memory, in the second; any other record has the same buffer in both. Shaders read
through the first and write through both (translator patch 0034): the GPU's writes through the table are not recorded as
GPU writes (`runtime/vk/vk_exec_draw.c` `ssbo_info()`), so with the mirror alone the CPU never saw them, and a sync of a
page the CPU wrote copied the stale guest page over them. The game's compute passes write tree and foliage records this
way, and a record overwritten so drew a tree stretched into a column reaching the sky (Hyrule Field, Kakariko, Death
Mountain). A record a page entry names is never written by the CPU: a new mapping takes a new record, and a record goes
back to the free list only once the GPU finished every submission issued while a page could still name it
(`vk_completion_add`). The one write to a record in use is the GPU's own, in queue order, when a pool's new mirror takes
its record over (`vk_gpa_flip_cmd`): what was queued before it reads the old addresses, what comes after it the new
ones. Page and L1 entries are single 32-bit stores, so a shader that reads the table while it changes sees the old
mapping or the new one, never half of each. The CPU keeps a copy (shadow) for its reads: the GPU copy is often device
memory behind the PCIe BAR, slow to read back.

An L2 block whose last page is unmapped leaves its L1 entry at once (`blocks_release`) and serves another 16 MiB range
once the GPU finished every submission issued while a shader could still have read the old L1 entry: until then it holds
only zeros, which read as unmapped. The game's allocator hands out pool addresses that climb through the 64 GiB alias
region, so blocks kept for good ran out after 16 GiB of ranges ever mapped (39 minutes of play, six areas), and every
pool created after that stayed out of the table: its pointers read zeros, as before the table existed. A range mapped
again while its block still waits takes that block back (`block_take`), so a pool freed and made again at its address
within a frame holds no second block.

`NX2SARIA_GPA=0` turns the table off: the translator is told the host has no device addresses (those accesses read 0).
`NX2SARIA_GPA_EMPTY=1` keeps it empty, so every access through it reads the zero buffer (diagnosis). Mirrored pools are
not synced for the table's readers (`runtime/vk/vk_exec_draw.c` `ssbo_info()` says why). With shadow storage the pools
are copies of guest memory, which a guest submission that binds the table (`runtime/vk/vk_exec_draw.c` `ssbo_info()`)
brings up to date with the pages the CPU wrote, once, before the part is submitted
(`runtime/vk/vk_memory.c` `vk_mem_shadow_sync()`). There the twin of a mirrored pool is its shadow copy, not guest
memory, and what a shader stores through the table is not noted as a GPU write: it does not come back to guest memory,
and the next whole copy-in of its page overwrites it. `runtime/vk/vk_memory.c` "Pages a shader stores into through the
device address table" has the sequence, what was measured, and the switch that keeps the stores
(`NX2SARIA_SHADOW_TABLE_STORES`).

- `L2_N`: pages per block: 16 MiB.
- `BLOCKS`: L2 blocks: 16 GiB of mapped GPU address space; block 0 stays zero.
- `RECS`: record 0 means none.
- `REC_WORDS`: words per record.
- `G`:
  - `int state`: 0 untried, 1 on, -1 off.
  - `volatile uint32_t *gpu`: the mapped table.
  - `uint32_t *cpu`: its shadow.
  - `uint32_t nblocks`: L2 blocks ever handed out (block 0 is the zero block).
  - `uint16_t blk_pages[BLOCKS]`: pages each block maps.
  - `uint16_t blk_l1[BLOCKS]`: the L1 entry that names it, or named it while it waits.
  - `uint8_t blk_wait[BLOCKS]`: off L1, waiting for the GPU (`blocks_release`).
  - `uint32_t blk_gen[BLOCKS]`: releases so far: a block taken back and released again has two callbacks.
  - `uint16_t free_blks[BLOCKS]`: empty blocks whose fence completed.
  - `uint64_t blk_returned`: blocks taken off L1 so far.
  - `uint32_t nrec`: records handed out, free ones aside.
  - `uint32_t *free_recs, nfree`: records whose fence completed.

### `create()`

- `.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT`: `vk_gpa_flip_cmd`.

### `block_take()`

A block for L1 entry l1, 0 when none is left: the one the entry gave up and that still waits for the GPU (it maps
nothing, and a shader that read the old entry finds the new pages in it), else a free one, else one never used.

- `if (blk) G.blk_wait[blk] = 0, G.blk_waiting--`: its callback finds it in use (`blk_free_cb`).

### `page_word()`

The L2 entry word of a page; 0 when its block does not exist and alloc is 0 or no block is left.

- `uint64_t l1 = page >> 12`: 4096 pages of 4 KiB per 16 MiB.

### `page_put()`

Stores record r (0: none) in the L2 entry word w and keeps the page counts of its block and of the table.

### `blk_free_cb()`

- `if (G.state > 0 && G.blk_wait[blk] && G.blk_gen[blk] == gen)`: not taken back since (`block_take`).

### `blocks_release()`

After pages of [p0, p1) were unmapped: the blocks of that range that map nothing leave L1, and go back to the free list
once the GPU finished the submissions issued until now and the one being recorded, as a record does.

### `out_of_blocks()`

`gpa_lock` held: a page found no block. Pointers into it read the zero buffer until its buffer is mapped again.

### `rec_alloc()`

- The record before any page names it: the page store below is ordered after these by the release fence.

### `vk_gpa_map()`

- `static int empty = -1`: `NX2SARIA_GPA_EMPTY=1`: nothing is mapped, every access reads the zero buffer (diagnosis).
- Pages the buffer holds whole: [lo, hi) rounded inwards (imports are aligned outwards, so this is all of them).
- `p |= L2_N - 1`: the rest of this 16 MiB has no block either; the next range may have one.

### `vk_gpa_unmap()`

- Pages another live buffer also holds whole (overlapping imports of the same guest memory) get its record back.
- `blocks_release(p0, p1)`: after the cover: a block another buffer's pages went back into stays.

### `twins_on()`

`NX2SARIA_GPA_TWIN=0` (diagnosis): no twin, the writes go to the mirror alone, as before patch 0034.

### `rec_first()`

`gpa_lock` held: the first address of record rec (the buffer its pages read).

### `pages_move()`

`gpa_lock` held: the pages of [p0, p1) that name `old_rec` (or none) name r from here on.

### `remap()`

- The twin: the buffer the pages named before (the pool's imported buffer), so writes reach guest memory too.
- `if (!r)`: no record left: the pages keep the old one, and the caller its number.

### `vk_gpa_flip_cmd()`

Records into cb the store that hands record rec over to buf, a pool's new VRAM mirror: from that point of the queue on
the pages that name rec read buf, and their twin is the buffer they read until then (`keep_twin`, with twins on) or buf
again. A change of the table made on the CPU is seen at once by every shader still running, the ones queued before the
mirror's first copy included, which would then read or write the mirror before anything had filled it; this store runs
where the command stands, after that copy (`runtime/vk/vk_memory.c` `vk_mem_mirror_prologue()`). The caller puts
barriers around it. No new record is taken, so there is none to run out of. The shadow takes the new words now: the CPU
never reads a record's addresses back for a record handed over this way. 0 when nothing was recorded.

### Probe-vkmem

What the table's own memory, as the GPU has it, resolves addr to: the device address a shader reads there and the one it
writes as well. 0 when no page maps addr. Slow (the table may be device memory), and only meaningful with no submission
in flight.

### `vk_gpa_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): device address table records and L2 blocks in use (those
waiting for a fence included), against their fixed sizes (when they run out, shaders read zeros).

## `runtime/vk/vk_gpa.h`

The device address table (`runtime/vk/vk_gpa.c`): GPU address pages to buffer device addresses for shaders' global
memory accesses.

- `VK_GPA_CB_SLOT`: res.json `cb_slot` of the table (translator ResourceManager.DeviceAddressTableCbSlot).
- `vk_gpa_wanted()`: the device has buffer device addresses and `NX2SARIA_GPA` is not 0.
- `vk_gpa_wanted_ctx()`: the same during device creation, before `vk_ctx()` returns it.
- `vk_gpa_on()`: the table exists (created on first use).
- `vk_gpa_info()`: the whole table, for a storage descriptor.
- `vk_gpa_map()`: Maps the pages [lo, hi) holds whole to buf, whose byte 0 is GPU address `buf_lo`. Returns the record
  (0: not mapped).
- `vk_gpa_unmap()`: Unmaps the pages of [lo, hi) that name rec; cover (may be NULL) gives the record of another live
  buffer that holds a page whole, or 0. rec is reused once the GPU finished the submissions issued until now.
- `vk_gpa_clear()`: Unmaps every page of [lo, hi), whatever record it names (pages of a virtual pool whose physical
  memory goes away).
- `vk_gpa_remap()`: Moves the pages of [lo, hi) that name `old_rec` (or none) to buf (a pool's VRAM mirror), with
  `old_rec`'s buffer as their twin, which shaders write too; returns the new record.
- `vk_gpa_remap_plain()`: The same without the twin: the pages name buf alone (a pool whose VRAM mirror is evicted goes
  back to its imported buffer).
- `vk_gpa_flip_cmd()`: Records into cb the store that hands record rec over to buf (a pool's new VRAM mirror) in queue
  order: what is queued after it reads buf through rec's pages, with the buffer they read before as the twin
  (`keep_twin`) or buf again; what was queued before it sees no change. 0 when nothing was recorded (no record, no
  table).
- `vk_gpa_probe()`: --probe-vkmem: the device addresses the table, read from its own memory, gives a shader for addr:
  the one it reads and the one it writes as well. 0 when no page maps addr.

## `runtime/vk/vk_dump.c`

`NX2SARIA_DUMP=<frame>`: a frame dump for the Vulkan back end, which RenderDoc cannot capture (it hides
`VK_EXT_external_memory_host`). Between presents frame-1 and frame, the replay's command buffers also copy, after each
render pass, its colour and depth targets and, after each dispatch, the storage images it writes into host buffers; the
completion thread turns them into 8-bit PNGs once the submission is done. 3D textures (LUTs, volumes) sampled or written
in the frame are copied once each, slices side by side. Files in `NX2SARIA_DUMP_DIR` (default "dump"):

- `pass_<seq>_t<i>_<w>x<h>_<vkformat>_<name>.png`: colour target i (td: depth; `img<slot>`: storage image; `lut<id>`:
  3D; a BC target drawn through a block view: its blocks as that view).
- `..._tm.png`: float formats: x/(1+x) of the same image (the plain one clamps); in both, a pixel with a NaN channel is
  magenta.
- passes.txt: per pass or dispatch: targets, draws, each distinct pipeline + texture set with its shaders' SPIR-V hashes
  (`sh_<hash>.spv` written once) and the bound textures with their samplers; draw preparation rejections by reason while
  the capture ran.
- images.txt: per image: channel min/max, distinct texel values over the image and over its top quarter.

Sampled 8-bit 2D textures up to `TEX2D_CAP` texels are copied once each as `pass_<seq>_tex<id>_...` at the end of the
pass that samples them; texture lines carry the storage address, mirror state, upload count and the image's and slot's
swizzle. Copies and texture clears are logged as "copy/clear after pass N". `NX2SARIA_DUMP_LOG=<from>` also logs the
frames from..frame-1 to passes.txt, with PNGs only of targets up to 64K texels. `NX2SARIA_DUMP_BUF=<addr>:<size>[:...]`
snapshots one GPU buffer range after every logged pass (`buf_<pass>.bin` and bufs.txt, see `buf_snap`). PNGs are opaque
(alpha dropped). Integer formats are scaled by the image maximum, depth by its min..max range, SNORM as x/2+1/2, UNORM
and sRGB bytes written as stored. Caps: `MAX_PASSES` passes copied, `PNG_CAP` bytes of PNGs.
`NX2SARIA_DUMP_TEX=<id>|<w>x<h>[,...]`: those texture slots, or 2D textures of that size, are copied where first
sampled, whatever their format. `NX2SARIA_DUMP_UBO=<n>`: each listed pipeline also prints the first n vec4 of its user
uniform blocks (c3 and up; 1 means 16, the cap is 4096), read from guest memory when the entry is listed. A block the
submission rewrites with UpdateUniformBuffer shows its value from before the submission
(`runtime/vk/vk_exec_draw.c` `upd_note()`). `NX2SARIA_DUMP_SKIP_RUNS=1`: a render pass with the same targets as the
render pass before it is logged but not copied (the terrain decals draw one pass each into the G-buffer and would use up
both caps). `NX2SARIA_DUMP_SHADERS=<hash>,...`: copy images only for passes using those shader hashes; metadata covers
the whole frame. `NX2SARIA_DUMP_RAW=1`: also save each copied image's native texel bytes as .bin, for numerical
analysis. `NX2SARIA_DUMP_KEY=<key>` (Windows): each press of that key in the game window dumps the next frame to
`NX2SARIA_DUMP_DIR/f<frame>`. Keys: pause (also 1), scrolllock, insert, home, end, pageup, pagedown, f2-f7, f9, f12 (F1,
F8, F10 and F11 are the menu, the bug capture, the overlay and fullscreen). The game still gets a key it has bound.

- `vk_exec_cs_storage_addr()`: `runtime/vk/vk_exec_draw.c`.
- `vk_exec_storage_addr()`: `runtime/vk/vk_exec_draw.c`.
- `vk_exec_raw_9e5()`: `runtime/vk/vk_exec_draw.c`.
- `INFLIGHT_CAP`: host buffers waiting for their submission.
- `MAX_ENTS`: distinct pipeline + texture sets listed per pass.
- `MAX_LUTS`: 3D textures and small sampled 8-bit 2D textures dumped per frame.
- `TEX2D_CAP`: texels of a sampled 2D texture that is dumped.
- `dump_frame`: `log_from`: `NX2SARIA_DUMP_LOG`, first frame logged to passes.txt without images.
- `base_dir`: `NX2SARIA_DUMP_DIR`; with `NX2SARIA_DUMP_WHITE` each attempt goes to `base_dir/f<frame>`.
- `white_last`: `NX2SARIA_DUMP_WHITE`: the frame dumped last (0: none), checked 3 presents on.
- `dump_after`: `NX2SARIA_DUMP_AFTER`.
- `dump_key`: `NX2SARIA_DUMP_KEY`: the Windows virtual-key code, 0 off.
- `seq`: copied: passes and dispatches of the dump frame whose images were copied.
- `skip_runs`: `NX2SARIA_DUMP_SKIP_RUNS`, `NX2SARIA_DUMP_UBO` (vec4 per block, 0 for none).
- `want_tex`: `NX2SARIA_DUMP_TEX`: slot id and 0, or width and height.
- `last_targets`: `targets_sig` of the last render pass; 0 after a dispatch.
- `pass_skip`: why the current pass copies nothing, or NULL.

### Formats

- `fmts`:
  - `F(D16_UNORM, "d16", 2, 1, 16, K_D16), F(D16_UNORM_S8_UINT, "d16s8", 2, 1, 16, K_D16)`: depth aspect of depth
    formats, as `vkCmdCopyImageToBuffer` writes it.

### `ufloat()`

Unsigned small float with a 5-bit exponent and mbits of mantissa (half: 10, R11G11B10: 6 and 5).

### Conversion on the completion thread

- `Job`:
  - `uint32_t w, h, d`: d: 3D slices, laid side by side in the PNG.

### `distinct()`

Distinct raw texel values among rows [y0, y1) of every slice.

### `write_images()`

- `uint8_t *out = malloc(n * 4), *isnan_ = calloc(n, 1)`: `isnan_`: a pixel with a NaN channel, drawn magenta.

### `job_done()`

- Raw captures retain the exact texel bytes. BC blocks always get a .bin because the PNG's integer mapping loses their
  bits; other formats get one when `DUMP_RAW` is enabled.

### Recording

### `copy_region()`

Records the copy of one subresource into a host buffer and queues its PNGs. Returns NULL, or why it was skipped. The
buffer is w x h x d texels of vf; the image region is `ext` image texels at depth plane z (differs for the blocks of a
BC image read as an uncompressed format of the block size, and for a 3D slice).

- `.imageExtent = ext}`: packed rows: a BC row of ext.width texels is w blocks.

### `NX2SARIA_DUMP_BUF=<addr>:<size>[:<off>:<stride>:<w>:<h>]`

After every pass and dispatch that is logged, the GPU buffer range is copied to a host buffer; when it differs from the
previous copy it goes to `buf_<pass>.bin`, and bufs.txt gets the changed bytes' extent plus, with the grid arguments,
one map per dword of the w x h records of `stride` bytes at `off` (each record a letter: its value's index in the table
under the map).

### `buf_snap()`

- `VK_MEM_PICK(s)`: the GPU's view of the buffer.

### `active()`

`passes_f` is NULL before a dump that opens its files when it starts (`open_files`).

### `vk_dump_draw_skipped()`

A draw can be rejected before it opens a render pass. Count it against the captured frame as a whole.

### `shader_hash()`

SPIR-V hash of a shader; the first time, its SPIR-V goes to `sh_<hash>.spv` and its guest program to `sh_<hash>.code`.

- `snprintf(path, sizeof path, "%s/sh_%016llx.code", dir, (unsigned long long)h)`: the guest program it came from.

### `queue_lut()`

- `luts`: queued for the next pass end or dispatch.

Per pass: distinct pipeline + texture sets, draws, 3D textures to copy at its end.

### `block_format()`

The uncompressed format of a BC format's block size, UNDEFINED for other formats: a BC texture is dumped as its raw
blocks (one texel per block), which shows at least whether it was ever written.

### `flush_luts()`

- A sampled depth texture copies its depth aspect (the colour aspect of a depth image read as zeros).

### `nvn_stage()`

The NVN stage numbers `VkExec`'s slot arrays use: 6 task, 7 mesh.

### `describe_bindings()`

The textures, images and samplers of info's bindings; 3D textures are queued for a copy.

- `if (r && bd->kind == GFX_RES_SSBO)`: where the address lives (c0: the stage's NVN slot table) and the stage's slots.
- `uint64_t pa; uint32_t ps`: an NVN2 compute push-constant pointer {address, size}
- `if (st == 5 || st == 6 || st == 7)`: the address and how far its backing span reaches (what a size-0 binding covers).
- `VkDescriptorBufferInfo cb`: "(copy)": a binding that only reads it uses the submit-time copy
  (`runtime/vk/vk_usnap.c`).
- `NX2SARIA_DUMP_UBO`: also the small storage buffers' guest bytes (GPU results as of the last write-back); per-view
  constants (exposure and such) live in such buffers.
- Storage image: no sampler.
- `int bw = 0`: a border wrap: its colour, as the sampler builder stored it.

### Mesh primitive counts

With `VkCtx.has_mesh_queries`, each mesh draw of the dumped frame runs inside a
`VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT` query, and `mesh_prims.txt` (written by `vk_dump_present`) lists what each
draw's mesh shader emitted, with its pass, program and depth state. A pipeline statistics query runs alongside: task and
mesh shader invocations, primitives the clipper got and gave out, and fragment shader invocations (a triangle with a NaN
or degenerate vertex is emitted but never clipped).

- `mq_n`: queries begun in the dumped frame.
- `mq_draw`: `mq_next`: the mesh draw `vk_dump_draw` saw last.

### `vk_dump_mesh_begin()`

- `if (!mq_n)`: the last dump's queries finished at its present.
- `mq_draw[q].pass = seq`: the number this pass gets when it ends.

### `mq_write()`

After `vk_sync_finish`: every query of the frame was submitted and ran.

- The counters come in the order of their flag bits: clipping invocations 0x20, clipping primitives 0x40, fragment
  invocations 0x80, task invocations 0x800, mesh invocations 0x1000.

### `vk_dump_draw()`

- `if (mesh)`: a mesh draw's per-draw parameters often live in its uniform blocks: draws that differ there are told
  apart.
- `for (int st = 0; st < 8; st++)`: every NVN stage: 6 is task, 7 mesh.
- `if (mesh)`: depth: test, write and the NVN function (`VkCompareOp` + 1: LESS 2, EQUAL 3, LEQUAL 4, GREATER 5, GEQUAL
  7).
- Target 0's blending as NVN numbers (`NVNblendFunc`, `NVNblendEquation`), which decides whether a quad covers or mixes.

### `target_line()`

- A block view: the plane's blocks, read as the view format (one texel per block).

### `render_copy_selected()`

Sets `pass_skip` for the pass or dispatch being logged and counts it when it copies.

### `vk_dump_pass_end()`

- `if (!passes_f) open_files()`: `NX2SARIA_DUMP_WHITE` and `NX2SARIA_DUMP_MOVIE` open them for each of their dumps.
- `dump_frame = frame_now()`: this pass and the rest of its frame.

### `vk_dump_dispatch()`

- `if (dump_ubo)`: `NX2SARIA_DUMP_UBO`: the NVN2 push constants as 64-bit words.
- `for (uint32_t i = 0; i < info->n_bindings && !pass_skip; i++)`: storage images the dispatch writes.
- `if (t->type == VK_IMAGE_TYPE_3D)`: a written volume: this copy replaces the sampled-3D one.

### `vk_dump_movie_frame()`

- `vk_dump_white`: `NX2SARIA_DUMP_WHITE=1` (diagnosis): no frame is named; from frame 1800 each present looks at the
  thumbnail of the frame three presents back (`runtime/gfx/gfx_present.c` `vk_thumbs_*`), and the first one whose
  picture left of the overlay and above the dialogue box is blank white makes the dump take the next frame (white
  flashes come in runs of several frames).

  `runtime/vk/vk_backend.c` asks for a thumbnail of every present while it is set.
- `movie_k`: `NX2SARIA_DUMP_MOVIE=<k>:<frame>[,<frame>...]` (diagnosis): the k-th movie of the run (counted from 1 as
  each video decoder comes up, `runtime/hle/ov_movie2.c` `ov_initialize()`) asks for a dump of the next present once it
  has shown each listed frame, so a movie's picture can be followed through the game's passes at chosen moments. Each
  dump has its own folder, `base_dir/f<present>`.
- `key_mode`: `NX2SARIA_DUMP=key` (`vk_dump_init`).
- `movie_req`: set by the movie's thread, taken by `vk_dump_present`.

### `open_files()`

- `dump_file`: `NX2SARIA_DUMP_FILE=<path>` (diagnosis): every 8th present looks for that file (a relative path starts in
  the game folder); when it is there, the present deletes it and asks for a dump of the next present, so a shell or a
  test script takes dumps at the moments it chooses, as many as it likes. A number in the file (up to 8) dumps that many
  consecutive frames, for what a frame hands to the next. Each dump has its own folder, `base_dir/f<present>`.
- `file_more`: consecutive frames still to dump after the one named.

passes.txt and images.txt in dir (a new folder per `NX2SARIA_DUMP_WHITE` attempt).

- `NX2SARIA_DUMP_BUF` with dumps named as the run goes (key, white, movie): the log goes with each dump's folder.

### `vk_dump_init()`

- `NX2SARIA_DUMP_AFTER=<address>`: the first render pass whose depth target lives at that address and the rest of its
  frame (a target drawn once, at an area load).
- `NX2SARIA_DUMP=key`: no frame is named; each press of Insert (`input_dump_key`: unbound in the game's layout, and read
  on Windows even while another window has the focus) dumps the frame after it into `NX2SARIA_DUMP_DIR/f<frame>`, as
  often as it is pressed: for a glitch that comes and goes, caught by the player while it is on screen.
- `NX2SARIA_DUMP_KEY`: a key of ours, also outside `NX2SARIA_DUMP=key`.
- A dump named as the run goes opens it with its folder (`open_files`); a named frame's folder exists already.

### `key_pressed()`

`NX2SARIA_DUMP_KEY`: its key went down since the last present (the game window has the focus).

### `thumb_white()`

The thumbnail of frame f is blank white left of the overlay and above the dialogue box: -1 not available, 0 no, 1 yes.

### `open_frame()`

Publish the selected frame and its output files together to the recording hooks.

### `white_check()`

- `static int run`: consecutive white thumbnails: white runs are mostly 1-2 frames, too short for a dump 4 frames on.
- `if (run >= 3)`: frames frame-5..frame-3 white: the run is likely still on 4 frames later.

### `vk_dump_present()`

- `if (dump_white && white_last && frame == white_last + 3)`: was the dumped frame itself white?
- `if (key_mode && dump_frame == UINT64_MAX)`: `NX2SARIA_DUMP=key`: a new press of Insert names the next frame.
- `if (dump_key && dump_frame == UINT64_MAX && key_pressed())`: `NX2SARIA_DUMP_KEY`: our key names the next frame.
- `open_files()`: before the frame is named: its passes write to them.
- `vk_sync_finish()`: the frame's copies are converted and written.
- `if (dump_white || movie_k || key_mode || dump_file[0] || dump_key)`: ready for another dump (`vk_dump_present` checks
  a white one's thumbnail 3 presents on).
- `if (file_more > 0)`: `NX2SARIA_DUMP_FILE` with a count: the next frame too.

## `runtime/vk/vk_crumbs.c`

GPU breadcrumbs for a lost device (`runtime/vk/vk.h` `vk_crumb()`). `VK_AMD_buffer_marker` writes a 32-bit code into a
buffer from the command stream: at `TOP_OF_PIPE` once the command processor reaches the marker, at `BOTTOM_OF_PIPE` once
every command before it has finished. Each crumb writes both, into two words of one host-coherent buffer, so after a
device loss word 0 names the newest point the GPU reached and word 1 the newest point it had finished: the hang lies
between them. A code is the crumb kind in the top 4 bits and its number in the low 28. The markers cost two small writes
per crumb, and crumbs mark submissions, upload batches, ASTC chunks and presents, not draws.

Pass marks (`runtime/vk/vk.h` `vk_pass_mark_begin()`) use words 2 and 3 of the same buffer: the replay marks the start
and the end of every render pass and dispatch, and a ring of records says what each mark was. The design: a mark per
pass and dispatch, numbers in a ring of 4096, text only at the device loss.

`vk_pass_mark_cmd` also runs on the recorder thread (`runtime/vk/vk_rec.c` `R_PassMark`), where `runtime/vk/vk.h`'s
macro would turn the timestamp into a new packet for the part being recorded: a second producer on the ring, which
corrupts it ("`vk_rec`: unknown packet" with `NX2SARIA_PASS_TIMES=1` on NVIDIA, where the recorder is on). The command
goes straight into cb.

- `words`: 0, 1: crumbs; 2, 3: pass marks; 4, 5: draw marks.
- `vk_pass_marks_on`: Pass marks (below).
- `pm_amd`: marks into words 2 and 3.
- `pt_pool`: pass times (below): two timestamps per ring slot.
- `pt_timed`: the slot's mark writes timestamps; cleared once they are read.

### Pass marks

- `ring`: Cost per pass or dispatch: about twenty stores into the ring and two commands, one at the start and one at the
  end, each a marker write or a checkpoint. Per draw nothing, except that the branch of
  `runtime/vk/vk_exec_draw.c` `prepare_draw()` that records changed pipeline state also notes the shaders of a pass's
  first draw. A full barrier already precedes every pass and dispatch (`runtime/vk/vk_exec.c` `vk_exec_pass_begin()`,
  `runtime/vk/vk_exec_draw.c` `op_dispatch()`), so the end-of-pipe write adds no wait.
- `seq`: marks made; only the replay thread writes it.

### `vk_pass_marks_want_nv()`

`NX2SARIA_PASS_CRUMBS`: unset, buffer marks where `VK_AMD_buffer_marker` exists; 1, also NVIDIA checkpoints; 0, neither.

### `pass_marks_init()`

- Timestamp profiling needs pass labels, but no vendor breadcrumb extension.

### Draw marks

(`NX2SARIA_DRAW_CRUMBS=1`, off by default; with `VK_AMD_buffer_marker` only) A pass mark names the pass the GPU was in
and the shaders of its first and last draw. The losses reported on RDNA 2 cards end inside one scene pass of 30 to 100
draws, and which draw the GPU never finished is not known. With this on, the replay writes a mark each time the shaders
or the pipeline state change inside a pass (`runtime/vk/vk_exec_draw.c` `prepare_draw_ex()`), into words 4 and 5 of the
breadcrumb buffer as a pass mark goes into words 2 and 3, and a ring keeps the pass, the draw's index in it and the two
shaders of each. After a loss word 5 names the last mark before which every draw had finished: the draw that hung was
recorded after it, with its shaders or those of one of the few marks up to the one in word 4. It costs two marker writes
per change, a few thousand a frame, so it is for the player who can reproduce a loss, not for everyone. The marks are
numbered as they are recorded, by the replay thread and by its workers when a pass's chunks are recorded side by side
(`runtime/vk/vk_exec.c`): the number is taken atomically and each number has its own slot of the ring, so no two threads
write one. The two marks the GPU names are exact either way. The numbers between them are in queue order only where one
thread recorded the pass; with workers some of them belong to chunks that ran earlier, which the report says.

- `VK_DRAW_MARKS`: power of two.
- `dseq`: draw marks made; any recording thread takes the next number.

### `vk_draw_mark()`

- `if (!id) id = __atomic_add_fetch(&dseq, 1, __ATOMIC_RELAXED)`: 0 means no mark.

### `vk_pass_mark_cmd()`

- `if (end == 2)`: a draw mark (`vk_draw_mark`): the same two points, into its own words.

### `NX2SARIA_GPU_PASSES=1`

(diagnosis, needs the pass marks): every `GP_PERIOD` presents the render passes and dispatches of `GP_FRAMES` frames get
GPU timestamps around them. Some presents later the results are summed by what the pass is (targets, shaders) and the
most expensive are logged in ms per frame. The timestamps run on the replay's command buffers, through the recorder like
the marks.

- `GP_LATER`: presents after the capture before the results are read: the GPU is done with them by then.
- `GP_KIND_PART`: The gap ledger: what the timed frames' parts spend outside their passes and dispatches. A part
  (`runtime/vk/vk_sync.c`: one command buffer of a submission) takes a slot of its own, kind `GP_KIND_PART`, with a
  timestamp at its start and its end. A gap runs from the part's start or the end of a mark to the start of the next
  mark or the part's end; `vk_gpu_gap_note` names the kinds of commands the replay records in the gap it is in (copies,
  clears, the upscaler, the write-back merge). A gap nobody noted holds barriers and state only. The report sums the
  gaps by the kinds noted in them. A pass split over two parts (`vk_sync_split`) keeps its own time, the wait for the
  second part included, and makes no gap.

  beside `VK_MARK_PASS` and `VK_MARK_DISPATCH`.
- `GpGap`: query numbers in `gp_pool`; kinds: bits of `VK_GAP_*`.
- `gp_in_mark`: between a mark's start and its end.
- `gp_pool`: `gp_prims`: mesh primitives generated per pass (with `meshShaderQueries`).
- `gp_stats`: fragment shader invocations per pass.
- `gp_state`: state 0 not set up, 1 on, -1 off; phase 0 idle, 1 capturing, 2 waiting.
- `vk_exec_mesh_counts()`: `runtime/vk/vk_exec_draw.c`.

### `gp_shader_hash()`

The hash `NX2SARIA_SKIP_SHADERS` takes and the frame dump prints: FNV-1a over the shader's SPIR-V.

- `uint64_t h = gfx_shader_spirv_fnv(s)`: kept by the shader: nothing is decoded here.

### `gp_resolve()`

- `GpRes`: q: query in `gp_pool`, `UINT32_MAX` until written.
- `gp_nwaited`: atomic: the game's threads add.

Result points (`gpu_passes`): where in a submission's GPU time the results the CPU waits for are done. The replay notes
a result (`vk_exec_note_results`: an event, a counter or occlusion report, a GPU write of unmirrored memory, a mirror
write-back) as it records it; the next timestamp the capture writes after it (a pass's or dispatch's start or end, a
part's end) is when the GPU is done with it at the latest. The report compares that, for the submissions the game's wait
after a frame's first submission waited for (`vk_sync_recorded_wait`), with the submission's end: how much earlier the
wait could end if it ended at the results instead of at the whole submission's completion.

### `vk_gpu_gap_note()`

The kinds of commands noted since the last mark ended (bits of `VK_GAP_*`), for a lost device's report: the next mark
takes them as the commands recorded before it (`VkPassMark.gap`). Replay thread only, as the notes are.

### `gp_begin()`

- `if (gp_part != UINT32_MAX) gp_gap_close(gp_n * 2)`: the gap before this mark ends at its start.

### `gp_end()`

- `if (gp_part != UINT32_MAX) gp_gap_from = i * 2 + 1, gp_gap_kinds = 0`: the next gap starts at this mark's end.

### `gp_tick()`

One timestamp of `gp_pool` in ticks; 0 when it is not there.

### `gp_report_gaps()`

The gaps of the timed frames, summed by the kinds noted in them, and the parts' own time for comparison with the perf
line's GPU time (which counts the mirror prologues too and leaves out the time between parts).

- `for (int k = 0; k < n && k < 12; k++)`: the most expensive sets first.

### `gp_report_results()`

The result points of the timed frames (`vk_gpu_passes_result`), for the submissions the game's wait waited for.

- `for (uint32_t i = 0; i < gp_n; i++)`: the submission's parts: its first start, its last end.

### `gp_report()`

- `if (l->kind == GP_KIND_PART)`: a part of a submission: for the gap ledger's line only.
- A pass of many draws and its neighbours differ in the draw count: the sum is by targets and shaders alone.
- The mesh workgroup counts of the indirect mesh draws (`runtime/vk/vk_exec_draw.c` `mesh_ring_entry()`).

### `set_at_frame()`

`NX2SARIA_SET_AT=<frame>:<key>=<value>[;...]`: settings changed by a test run while the game plays, as the menu would.

### `vk_pass_mark_begin()`

- `if (!id) id = ++seq`: 0 means no mark.
- `m->gap = gap_now, gap_now = 0`: what was recorded between the mark before and this one.
- Timed unless the slot's previous mark is still waiting to be read (its queries are not reset yet).

### `vk_pass_mark_end()`

- `gap_now = 0`: the gap after this mark starts empty.

### `shader_id()`

The first 8 bytes of the shader's input id (`runtime/gfx/gfx_shader.c`), the start of its stored input's name
`<cache>/in/<id>.in`.

### `describe_mark()`

- The mesh draws among them: the RX 6000 losses whose marks named a pass were in passes that start with one, and whether
  a pass held any (and how many took their count from the GPU) is what tells such a loss from another.

### `nv_report()`

vkGetQueueCheckpointData2NV, else vkGetQueueCheckpointDataNV, on the graphics queue (the replay's): one entry per stage
with the last checkpoint that stage executed. A checkpoint is the mark's id times 2, plus 1 at its end, so the
top-of-pipe entry names the last start or end the GPU reached, and the bottom-of-pipe entry the last one before which
everything had finished: at a start, that is the end of the mark before it.

### `gap_text()`

The kinds of bits (`VK_GAP_*`) as text; "barriers and state only" when none was noted.

### `report_between()`

A GPU that finished the last mark it started stopped between two marks, where the two lines of `vk_pass_marks_format`
name nothing: in a player's log both were one compute dispatch, with 51 later marks made and none of them started. This
adds the mark that never started and what the replay recorded between the two (the gap notes: copies, clears, the
upscaler, write-backs), or what it recorded after the last mark of all.

### `draw_marks_report()`

Words 4 and 5 (`vk_draw_mark`): the draws the GPU had not finished were recorded after the mark in word 5, so its
shaders and those of the marks up to the one in word 4 are the candidates; up to `DRAW_MARK_LINES` of them are listed.

- `if (reached < finished || reached - finished + 1 > n)`: the mark the GPU last reached, when the list left it out.

### Pass times

(`NX2SARIA_PASS_TIMES=1`): GPU time per render pass and dispatch, for GPUs no vendor profiler supports (RGP covers RDNA
only, so an RX 580 has none, and RenderDoc cannot capture this program). Every pass mark also writes a timestamp at its
two points (`TOP_OF_PIPE` before the pass, `BOTTOM_OF_PIPE` after it), into the query slot pair of its ring slot.
`pass_times_poll`, on the replay thread at each new mark, takes in the marks whose submission has finished, resets their
slots on the host (`hostQueryReset`) and sums their time per kind of pass: its size, attachment formats and its first
fragment (or compute) shader. Every 600 presented frames the log lists the passes with the most GPU time per frame.
Passes overlap on the GPU, so the times are each pass's span, not its exclusive share; their order is what matters. A
slot whose mark was not read yet when the ring comes round is not timed (`pt_timed` 2) for that turn.

- `PtSum`:
  - `uint32_t vw, vh`: the largest viewport its draws set.
- `pt_read`: the newest mark id taken in.

### `pt_add()`

- `pt_lost++`: table full: counted in the total only.

### `pass_times_poll()`

Replay thread (`vk_pass_mark_begin`).

- `if (!m->fence || m->fence > done) break`: its submission is still being recorded or running.
- The submission finished: both timestamps are in, or one was never written (a pass whose end was not recorded), which
  leaves that mark out.
