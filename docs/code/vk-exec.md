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
