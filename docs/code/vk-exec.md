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

## `runtime/vk/vk.h`

Vulkan-internal header of the graphics layer. Host code only; the guest never sees it.

- `vk_vma_create_image()`: Every VMA allocation goes by the device-local heaps' live budgets
  (`runtime/vk/vk_vma_budget.c`): a heap within the reserve of its budget takes nothing new while another heap can. Each
  allocation is also counted under its owner, by the source file that made it, until it is freed through the wrappers
  below (`vk_vma_owner_bytes`, for the memory report: `runtime/vk/vk_memory.c` `owner_log()`).
- `vk_vma_moved()`: allocations placed outside a full heap so far.
- `enum { VK_OWN_TEXTURES, ... }`: The owners of VMA allocations (`runtime/vk/vk_vma_budget.c` `owner_of()` says which
  files make each one's).
- `vk_vma_owner_bytes()`: The bytes of an owner's live allocations on the device-local heaps (local 1) or on the others
  (local 0). No lock.
- `#include "nvn_obj.h"`: `pool_t`, `tex_t`, `view_t`, `samp_par`, `NVNcopyRegion` (memory and textures block).
- `#include "gfx.h"`: `GfxShader`, `GfxShaderRes`, `GfxShaderAnswers` (pipelines block).
- `VkCtx`: Required (device choice fails without them): Vulkan 1.3; `VK_KHR_swapchain`, `VK_KHR_push_descriptor`,
  `VK_EXT_depth_clip_control`, `VK_EXT_external_memory_host`; features `timelineSemaphore`, `dynamicRendering`,
  synchronization2, `shaderStorageImageWriteWithoutFormat`, `depthClipControl`, `shaderInt64`, `samplerAnisotropy`,
  `independentBlend`, `geometryShader`, `textureCompressionBC`. Optional (flag set in `VkCtx` when enabled): the `has_*`
  members.
  - `uint32_t gfx_family`: graphics + compute + presentation on win32.
  - `uint32_t xfer_family`: a transfer-only family when the GPU has one, else `gfx_family`.
  - `VkQueue gfx_queue`: graphics + present.
  - `VkQueue xfer_queue`: equals `gfx_queue` when `xfer_shared`.
  - `int xfer_shared`: 1: no separate queue; serialise with `gfx_queue`.
  - `VkSemaphore timeline`: one timeline semaphore; values are issued by `runtime/vk/vk_sync.c`.
  - `volatile int32_t validation_errors`: errors seen by the messenger (`NX2SARIA_VK_VALIDATION=1`).
  - `int validation`: layer active.
  - `has_memory_priority, has_pageable`: `VK_EXT_memory_priority`, `VK_EXT_pageable_device_local_memory`
    (`NX2SARIA_VRAM_PAGEABLE`).
  - `has_nv_checkpoints`: `VK_NV_device_diagnostic_checkpoints` (`NX2SARIA_PASS_CRUMBS=1`, `runtime/vk/vk_crumbs.c`).
  - `has_astc_ldr`: `textureCompressionASTC_LDR` (enabled when reported).
  - `has_calibrated`: `VK_KHR/EXT_calibrated_timestamps` (GPU idle split in the perf line).
  - `has_hdr_metadata`: `VK_EXT_hdr_metadata` (`runtime/vk/vk_hdr.c` `vk_hdr_metadata()`).
  - `has_mesh_shader, has_vrs`: `VK_EXT_mesh_shader` with `taskShader` and `meshShader` (NVN2 task/mesh stages).
  - `has_maximal_reconvergence`: `VK_KHR_shader_maximal_reconvergence`
    (`runtime/vk/vk_pipeline.c` `spirv_reconverge()`).
  - `has_pipeline_binary`: `VK_KHR_pipeline_binary` with `VK_KHR_maintenance5`
    (`runtime/vk/vk_pipeline.c` `pipe_create()`).
  - `has_low_latency2`: `VK_NV_low_latency2` with `VK_KHR_present_id`, which it needs (`runtime/pacing/vk_lowlat.c`).
  - `has_present_id`: ... and the `presentId` feature: presents may carry a frame's number.
  - `has_anti_lag`: `VK_AMD_anti_lag` with its `antiLag` feature (`runtime/pacing/vk_lowlat.c`).
  - `lists_low_latency`: the driver lists one of the two, left out: the run started with the mode off.
  - `has_mesh_queries`: `meshShaderQueries`, kept while `NX2SARIA_GPU_PASSES` or `NX2SARIA_DUMP` is set.
  - `int mesh_fallback`: task and mesh programs run as compute passes and are drawn from buffers (`mesh_fallback`: auto
    as `runtime/vk/vk_mesh_path.h` decides; `runtime/vk/vk_exec_draw.c` "mesh fallback").
  - `int mesh_fallback_default`: the fallback is this device's default (`mesh_fallback` = auto chose it), not forced by
    `mesh_fallback` = 1: `mesh_fallback_batch` = auto batches there.
  - `int mesh_emulation`: they are drawn by the mesh emulation (`mesh_emulation` = 1 where `runtime/vk/vk_mesh_path.h`
    lets it act; `runtime/vk/vk_exec_draw.c`, `runtime/vk/vk_pipeline.c`).
  - `int feedback`: Feedback loops ([maxwell.md](../maxwell.md) 1.9, `runtime/vk/vk_exec_draw.c` `feedback_prepare()`):
    a draw samples a target of its open pass that an earlier draw of the pass wrote. `NX2SARIA_FEEDBACK=0`:
    `VK_FEEDBACK_OFF` (sampled in GENERAL without a barrier, the old behaviour); `VK_FEEDBACK_LAYOUT` with
    `VK_EXT_attachment_feedback_loop_layout` and `dynamicRenderingLocalRead` (attachable images get feedback usage, a
    by-region barrier inside the pass); else (or `NX2SARIA_FEEDBACK=restart`) `VK_FEEDBACK_RESTART` (the pass ends and
    restarts behind the full barrier).
  - `int feedback`: `VK_FEEDBACK_*`.
  - `uint32_t min_imported_host_alignment`: `VK_EXT_external_memory_host`.
  - `int device_index`: index in `vkEnumeratePhysicalDevices`.
- `VkCaps`: `runtime/vk/vk_caps.c`: fallbacks for GPUs other than the RX 9070, decided at device creation.
  - `int float64`: `shaderFloat64` enabled; 0: shaders are translated with softfloat64.
  - `uint32_t sbuf_align`: nonzero: storage buffers bind at their address rounded down to this.
  - `uint32_t sg_size`: `requiredSubgroupSize` on `sg_pin_stages` (32).
  - `VkShaderStageFlags sg_pin_stages`: 0: no pin, stages run at the device's `subgroupSize`.
  - `int gpl`: `VK_EXT_graphics_pipeline_library` enabled with fast linking (`NX2SARIA_GPL=0/1`; unset: off on AMD's
    driver for RDNA 2 and older).
  - `int amd`: an AMD GPU (vendor 0x1002), any driver.
  - `int amd_sc`: AMD's Windows driver branch for RDNA 1 and 2 ("AMD proprietary shader compiler").
  - `int amd_llpc`: AMD's own driver for RDNA 3 and newer (its `driverInfo` names LLPC).
  - `int mesa`: a Mesa driver the game runs on: RADV, ANV or NVK.
  - `uint32_t driver_id`: the device's VkDriverId.
- `vk_caps_driver()`: first, before `vkCreateDevice`: `vk_caps.amd`, `amd_sc`, `amd_llpc`, mesa, `driver_id`.
- `vk_caps_summary()`: The card and its driver in one phrase, for a fatal error's summary line ("" before
  `vk_caps_init`). Any thread.
- `vk_caps_features()`: before `vkCreateDevice` (`NX2SARIA_VK_NO_FLOAT64` clears `shaderFloat64`).
- `vk_caps_gpl()`: Before `vkCreateDevice`, with the driver's graphics pipeline library feature and properties: sets
  `vk_caps.gpl`, 1 = enable the extensions (`VK_KHR_pipeline_library`, `VK_EXT_graphics_pipeline_library`) and the
  feature.
- `vk_caps_init()`: after it: pretend limits, fallbacks, the boot report.
- `vk_ctx()`: NULL until `vk_init()` succeeded.
- `vk_card_prepare_text()`: the card the device is on and how it was chosen
  (`runtime/vk/gpu_choice.h` `gpuc_prepare_line()`), for the preparation modes' line; "" before `vk_init`.
- `vk_card_usable()`: 1 when the game can run on pd (the menu's Graphics card row asks it of the discrete cards); needs
  `vk_init`.
- `vk_init()`: idempotent; 1 on success, 0 on failure (reason logged with `rt_log`).
- `vk_selftest()`: 0 on success.
- `vk_device_lost_check()`: Call with the result of every queue submit, present, acquire or wait. Any result but
  `VK_ERROR_DEVICE_LOST` returns at once, and the caller goes on with its own handling. A lost device ends the run here,
  in one place: the first thread to arrive sets `vk_lost`, logs the GPU breadcrumbs (`vk_crumbs_report`) and what
  `VK_EXT_device_fault` reports (description, faulting addresses with their precision, vendor codes), then ends the
  process as `rt_fatal` does (the log written, a save commit under way finished) with exit code `RT_LOST_EXIT`
  (`runtime/dispatch.h`). `where` names the call for the fatal line. Every later thread stops in this call and never
  returns to touch the device; the process ends within `LOST_EXIT_MS` (`runtime/vk/vk_device.c`) even when the first
  thread hangs on a lock one of them holds.
- `vk_lost`: 1 from the moment a thread saw the device lost. The paths that would touch the device (the replay and its
  submit, the uploads, the present, the completion thread's callbacks) read it where they start and return without doing
  so.
- `vk_device_lost_poll()`: Asks the device whether it is lost with a call that has a result and needs no lock (the
  status of a fence nothing signals), and ends the run through `vk_device_lost_check` when it is. The completion thread
  calls it before it runs callbacks: a lost device may report the submissions' timeline as reached
  (`runtime/vk/vk_sync.c` `completion_main()`).
- `vk_sync_signal_note()`: `runtime/vk/vk_sync.c` "a lost device in the timelines' values": a host signal of
  `VkCtx.timeline` notes its value first; a read of the timeline above every value its signals carry ends the run as a
  lost device (`vk_sync_check_timeline`, any thread, `where` for the fatal line). `vk_sync_state_text`: the submissions'
  values and the two timelines' readings, one line without a lock; `vk_sync_lost_report` logs it
  (`runtime/vk/vk_device.c` `device_fault_report()`).

### GPU breadcrumbs

(`runtime/vk/vk_crumbs.c`): with `VK_AMD_buffer_marker`, each crumb has the GPU write its code into host memory twice,
when the command processor reaches it and when all work before it has finished. After a lost device the two codes
bracket the work that hung. Without the extension every call returns at once.

- `vk_crumbs_init()`: after the allocator exists.
- `vk_crumb()`: Records crumb kind with number n (a timeline value, a batch number; the low 28 bits are kept) into cb.
  Only the thread that records cb may call it: never into a replay part while the recorder thread owns it
  (`runtime/vk/vk_rec.h`).
- `vk_crumbs_report()`: logs the two codes and what they name.

### Pass marks

(`runtime/vk/vk_crumbs.c`): the replay (`runtime/vk/vk_exec.c` `vk_exec_pass_begin()` / `vk_exec_pass_end`,
`runtime/vk/vk_exec_draw.c` `op_dispatch()`) marks where each render pass and dispatch starts and where it ends, so that
a device loss names the pass the GPU was in. A mark is a number; a ring of `VK_PASS_MARKS` records keeps what it was.
With `VK_AMD_buffer_marker` the GPU writes the number of the last mark it started (`TOP_OF_PIPE`, before
`vkCmdBeginRendering` or the dispatch) and of the last it finished (`BOTTOM_OF_PIPE`, after `vkCmdEndRendering` or the
dispatch) into the breadcrumb buffer; with `VK_NV_device_diagnostic_checkpoints` the same two points are checkpoints,
read with `vkGetQueueCheckpointData(2)NV` after the loss. `NX2SARIA_PASS_CRUMBS`: unset, AMD buffer marks only; 1, also
NVIDIA checkpoints (the extension is enabled at device creation); 0, none.

- `VK_PASS_MARKS`: power of two.
- `VkPassMark`:
  - `uint32_t id`: the mark this record belongs to (0: never written).
  - `uint8_t kind`: `VK_MARK_*`.
  - `uint8_t ncolor`: colour attachments.
  - `uint32_t step`: index of the pass, or of the dispatch, within its submission, from 0.
  - `uint32_t w, h, d`: render area and layers; a dispatch: group counts (0 0 0: indirect).
  - `uint32_t draws`: draws recorded in the pass; `UINT32_MAX` until its end is recorded.
  - `uint32_t vw, vh`: the largest viewport a draw of the pass set (dynamic resolution draws in part).
  - `uint64_t fence`: the submission: the timeline value it signals.
  - `VkFormat color[8], depth`: attachment formats (`VK_FORMAT_UNDEFINED`: none).
  - `const GfxShader *first[2], *last[2]`: vertex and fragment shader of the first and last draw; a dispatch:
    `first[0]`.
  - `uint32_t gp`: `NX2SARIA_GPU_PASSES`: the timestamp pair of this mark plus one; 0 none.
  - `uint32_t layer`: `NX2SARIA_GPU_PASSES`: the first layer of the depth target (a shadow cascade).
  - `uint32_t gap`: bits of `VK_GAP_*`: what the replay recorded between the mark before and this one.
  - `uint16_t mesh, mesh_ind`: a pass's mesh draws, with a count and with the GPU's (DrawMeshTasksIndirect).
- `vk_gpu_passes_frame()`: `runtime/vk/vk_crumbs.c`, each present: `NX2SARIA_GPU_PASSES`, GPU time per pass and
  dispatch.
- `enum { VK_GAP_COPY_B2B, ... }`: `NX2SARIA_GPU_PASSES` also times what a part of a submission spends outside its
  passes and dispatches (`runtime/vk/vk_crumbs.c`, "the gap ledger"). `vk_gpu_passes_part`: `runtime/vk/vk_sync.c`, at
  the start (end 0) and the end (end 1) of each part's command buffer. `vk_gpu_gap_note`: the replay is about to record
  commands of this kind outside a pass or dispatch; units is their size (bytes for buffers, texels for images, 0
  unknown). Outside the timed frames the first does nothing and the second only notes the kind for the next pass mark
  (`VkPassMark.gap`, a lost device's report). Replay thread only.
  - `VK_GAP_GROUP`: a group of draws the replay workers recorded (`runtime/vk/vk_exec.c` `group_start()`): passes
    without marks.
- `vk_gpu_passes_fence()`: Result points (`NX2SARIA_GPU_PASSES`): the submission whose parts follow
  (`runtime/vk/vk_sync.c`), a result just recorded (`VK_RES_*` bits, `vk_exec_note_results`), a submission the game's
  wait after a frame's first submission waited for.
- `vk_sync_result_mask()`: `NX2SARIA_SUBMIT_FINISH_KINDS` as read.
- `vk_pass_marks_on`: Pass marks wanted and possible (`NX2SARIA_PASS_CRUMBS` and the extensions): set by
  `vk_crumbs_init`.
- `vk_pass_marks_want_nv()`: `runtime/vk/vk_device.c` `create_device()`, before `vkCreateDevice`: 1 when
  `NX2SARIA_PASS_CRUMBS` asks for NVIDIA checkpoints.
- `vk_pass_mark_begin()`: The next mark: records its start into cb (through the recorder thread when it owns cb) and
  returns its ring record, id, kind and fence set, for the caller to fill. NULL when pass marks are off. Replay thread
  only.
- `vk_pass_mark_end()`: records the end of mark m into cb.
- `vk_pass_mark_cmd()`: The recorder thread's side of both (`runtime/vk/vk_rec.c`): the marker and checkpoint commands
  themselves.
- `vk_pass_marks_read()`: AMD only: the last mark the GPU started and the last it finished (0: none); 0 without buffer
  marks.
- `vk_pass_mark_record()`: the ring record of mark id; NULL once a later mark took its slot.
- `vk_pass_marks_format()`: The report's text for a started and a finished mark, three lines without a trailing newline:
  which marks (source: the extension they came from), then each with its ring record. `vk_pass_marks_report` logs it;
  the probe checks it.
- `vk_pass_marks_report()`: `runtime/vk/vk_device.c` `device_fault_report()`: the AMD words and the NVIDIA checkpoints,
  formatted.
- `vk_draw_marks_on`: Draw marks (`runtime/vk/vk_crumbs.c`, `NX2SARIA_DRAW_CRUMBS=1` with `VK_AMD_buffer_marker`; off by
  default): a mark where the shaders or the pipeline state change inside pass m, before the draw with index draw in that
  pass, so a lost device's report names the shaders of the draws the GPU had not finished. From any thread that records
  draws (the replay and its workers): the mark takes its number atomically. Nothing while `vk_draw_marks_on` is 0.

### Window and present

(`runtime/gfx/gfx_present.c`). Host threads only. `vk_present_image`: blits the crop rectangle {x, y, w, h} of src
(NULL, or w/h <= 0: the whole w x h image), scaled to the swapchain extent, into the acquired swapchain image and
presents it. src is in layout, which is `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` (its writes made visible to the transfer
stage by the caller's barrier) or `VK_IMAGE_LAYOUT_GENERAL` (`runtime/vk/vk_texture.c` images: the present makes earlier
writes visible and orders later writes after the blit). All its writes must be submitted to `gfx_queue` before the call
(queue order does the rest). Starts the window on first use. A minimised window or a lost swapchain skips the frame.
Calls serialise on an internal lock.

- `vk_present_blit()`: `vk_present_image` split in two: `vk_present_blit` submits the blit and leaves the present
  waiting, `vk_present_flip` queues it. The next blit or a swapchain rebuild queues a waiting present first.
- `vk_fsr_record()`: FSR 1 on the present (`runtime/vk/vk_fsr.c`): records EASU (+ RCAS) of src's crop rectangle {x, y,
  w, h} into the rectangle out {x, y, w, h} of dst, a swapchain image in `TRANSFER_DST`; src is in GENERAL and fmt its
  format. 1 = EASU enlarged it, 2 = a bilinear blit shrank it and RCAS sharpened it (the picture is smaller than the
  crop), 0 = nothing recorded (the setting says bilinear, the sizes are equal, a shrink without sharpening, or FSR is
  not available): the caller blits.
- `vk_fsr_release()`: before the device goes away.
- `vk_color_record()`: Colour adjustment on the present (`runtime/vk/vk_color.c`): with `NX2SARIA_COLOR_*` away from
  their defaults, records a pass that writes src's crop rectangle {x, y, w, h}, adjusted, into an RGBA16F image of that
  size in GENERAL and returns it (the caller scales that one instead, crop {0, 0, w, h}); `VK_NULL_HANDLE` = nothing
  recorded. src is in GENERAL, fmt its format.
- `vk_color_release()`: before the device goes away.
- `vk_color_preset()`: The brightness, contrast, saturation and gamma values (as the settings write them) of a colour
  profile, NULL for custom or an unknown name (`runtime/vk/vk_color.c`).
- `vk_hdr_armed()`: HDR output (`runtime/vk/vk_hdr.c`). `vk_hdr_armed`: the HDR path is set up for this run (decided at
  the start). `vk_hdr_wanted`: `NX2SARIA_HDR` as the menu has it now. `vk_hdr_surface_format`: the HDR10 surface format
  when wanted and offered (1). `vk_hdr_set_active`: `runtime/gfx/gfx_present.c` says whether the swapchain is HDR10
  (`vk_hdr_active`). `vk_hdr_encode`: like `vk_color_record`, src's crop as PQ code values (RGBA16F, crop {0, 0, w, h}).
  `vk_hdr_ui_target` / `vk_hdr_ui_compose`: the in-game menu's RGBA8 target, then blended onto the swapchain image.
  `vk_hdr_to_srgb8`: 10-bit PQ pixels to sRGB bytes.
- `vk_hdr_needs_restart()`: the menu's HDR row: the value set takes effect at the next start.
- `vk_hdr_restart_for()`: the same for a value the menu's draft holds.
- `vk_hdr_off()`: probes that read back 8-bit swapchain pixels: SDR whatever the setting.
- `vk_hdr_native_picture()`: `vk_hdr_native`, and f is the format of the game's PQ picture.
- `vk_hdr_luminance_stats()`: Luminance statistics of an HDR10 shot inside rect {x, y, w, h}: the darkest pixel, the
  0.1st, 1st, 50th and 99th percentiles and the brightest pixel in nits, and the share of pixels under 0.01 nits
  (`runtime/vk/vk_hdr.c`).
- `vk_hdr_images_release()`: the swapchain has no HDR: the encode's images go (waits for the device); before the
  device goes away.
- `vk_hdr_support()`: The support buffer's HDR settings, five vec4 at 1440
  (`runtime/vk/vk_exec_draw.c` `support_fill()`; read by the game's tonemap with `shaders/hdr_tonemap.frag` spliced in,
  `runtime/vk/vk_hdr.c` `tonemap_splice()`).
- `vk_contact_strength()`: `runtime/vk/vk_contact.c`: screen-space contact shadows on the game's composed scene colour
  (`NX2SARIA_CONTACT_SHADOWS`).

  percent, 0 = off.
- `vk_contact_release()`: before the device goes away.
- `vk_present_swapchain_mem()`: The swapchain's images, their size and their pixels' bytes, for the memory report
  (`runtime/vk/vk_memory.c` `outside_log()`).
- `vk_ffx_vram()`: FSR's video memory for the memory report (`runtime/vk/vk_ffx.cpp`): the upscaling contexts', FSR
  3.1's and FSR 4's, with their count, and frame generation's in `*fg`; as the library reported each at its creation, 0
  where it did not say.
- `vk_queue_lock()`: Every submission to `ctx->gfx_queue` (and `xfer_queue` when shared) takes this lock;
  `vk_present_image` and the swapchain rebuild do. Recursive. Taking it first hands the replay's held submissions to the
  queue (`vk_sync_flush_held`); the raw forms leave them held, for `runtime/vk/vk_sync.c` itself.
- `vk_queue_trylock_raw()`: 1 when acquired.
- `vk_sync_flush_held()`: queue lock held: the held submissions go to the queue in one call (`runtime/vk/vk_sync.c`).
- `gfx_fit_rect()`: Copy of the last presented swapchain image, for probes: on, the next presents read the image back
  into a host buffer and wait for the GPU. `*px` stays valid until the next present; the format is the swapchain's.
  Returns 0 with no frame. The rectangle {x, y, w, h} of a dw x dh swapchain image a cw x ch picture is presented in
  (`runtime/gfx/gfx_present.c`, `NX2SARIA_ASPECT`); the rest is black.
- `VkGrab`: The next presented frame, as the window gets it with the overlay on it, for a bug capture
  (`runtime/diag/bugcap.c`): its pixels are copied (px, malloc'd, as the swapchain holds them) and cb gets them on the
  presenting thread right after that present; cb owns g and frees it with `vk_present_grab_free`.
  `vk_present_grab_rgba`: the pixels as RGBA bytes, an HDR10 picture converted to sRGB, into out (w * h * 4). One
  request at a time: 0 while one waits.
  - `int w, h`: the swapchain image.
  - `int bgra, hdr, a2r`: its bytes' order; an HDR10 swapchain, and whether it is A2R10G10B10.
  - `int image_w, image_h`: the game image this present showed.
  - `int scaler`: how it was scaled to the window: 0 not, 1 bilinear, 2 FSR 1, 3 bilinear + sharpening.
  - `float pic[4]`: where the picture sits in the swapchain image, in fractions: x, y, w, h.
- `vk_present_grab_cancel()`: the request is dropped: once this returns, cb has run or never will.
- `gfx_window_vk_extensions()`: The window system's part of device creation (`runtime/gfx/gfx_window_win.c` with Win32,
  `runtime/gfx/gfx_window_sdl.c` with SDL): the instance extensions a surface needs (their count, at most max), and
  whether a queue family of pd can present to it.
- `gfx_window_resize()`: programmatic resize; returns after the swapchain is rebuilt.
- `gfx_vsync_bind_counter()`: host-side stand-in for a bound event: incremented per tick.
- `gfx_window_focus_messages()`: `OE_MSG_FOCUS_STATE_CHANGED` pushes made by the window thread.
- `gfx_window_release_vk()`: `vk_shutdown` calls it first: destroys swapchain, surface, present objects.
- `vk_validation_errors_total()`: validation errors since the first `vk_init`, across `vk_shutdown`.

### Memory and textures (`runtime/vk/vk_memory.c`, `runtime/vk/vk_texture.c`)

Guest or host threads; every call needs `vk_init()`. The first nine have the `GfxBackend` signatures of
`runtime/gfx/gfx.h`; `runtime/vk/vk_backend.c` builds `gfx_vulkan_backend` from them.

- `vk_pool_init()`: CPU-visible: guest storage imported (`VK_EXT_external_memory_host`) as one `VkBuffer`; physical:
  device-local memory; virtual: a 64 KiB page table.
- `vk_pool_fini()`: drops images cut from the pool; waits for the GPU if it used the pool.
- `vk_pool_map_virtual()`: `nvn_mapping_t[n]`, checked by the front end.
- `vk_texture_register()`: texture descriptor slot (copied).
- `vk_sampler_register()`: sampler descriptor slot (copied).
- `vk_reg_replay_begin()`: `runtime/vk/vk_backend.c` "registrations in submit order": `runtime/vk/vk_sync.c` submit
  calls these around the replay of a submission of the game's commands. A registration the game makes in between waits
  and takes effect at the end.
- `vk_texture_release()`: TextureFinalize: the image stays for slots that still name its storage.
- `vk_guest_written()`: a host write write-watch may miss: textures there re-upload.
- `VkMemSpan`: GPU address -> backing (section 3.1). avail: bytes from offset that stay contiguous in buffer.
  `alloc/alloc_offset`: the device-local memory behind physical and (through the page table) virtual pool addresses,
  NULL for imported pools. cls: the pool class of the backing (`VK_MCLS_*`), for the traffic census.
  - `void *pool`: the imported pool record behind the span (NULL for device memory), for `VK_MEM_BOUND`.
- `enum { VK_MCLS_NONE, ... }`: Traffic census (`runtime/vk/vk_memory.c` `vk_mem_census()`): bytes the replay binds or
  copies, by pool class and use. Imported pools by their CPU access flag; DEVICE is physical memory (directly or through
  a virtual pool); SCRATCH the replay's transient copies. `CPU_READ` counts bytes host code reads from pool storage in
  place of the GPU (misaligned binds, byte indices, counter words); `GPU_WRITE` what copies, fills, counter reports and
  uniform updates write.
- `vk_mem_bound_note()`: `NX2SARIA_TRACE=perf` dirty census (`runtime/vk/vk_memory.c`): the replay notes each imported
  pool it binds as a vertex, index, uniform, storage or indirect buffer; at the start of every submit the pages the CPU
  wrote in such pools since the previous submit are counted from the shared page generations and logged as `mem: dirty`
  every 60 frames. Nothing when tracing is off: `VK_MEM_BOUND` is one flag test at the call site.
- `vk_dpa`: The `vkGetDeviceProcAddr` the runtime fetches device entry points with: the loader's, or Streamline's
  interposer when DLSS Frame Generation is up (`runtime/vk/vk_device.c` sets it right after the device is created).
  Functions fetched the other way bypass the plugins' hooks.
- `vk_gpu_busy_all()`: Every timed submission part's GPU duration summed, never reset: a reader differences it across
  frames (`runtime/pacing/dynres.c`).
- `vk_mem_dirty_submit()`: `vk_sync_submit`, under its lock, before the replay.
- `vk_mirror_on`: VRAM mirror of buffer-bound pools (`runtime/vk/vk_memory.c`). `vk_mirror_on`:
  `NX2SARIA_MEMORY=mirror|verify`, or the device class is discrete and `NX2SARIA_MEMORY` is unset or auto; set at device
  selection. The replay (submitting thread under `vk_sync_submit`'s lock) calls:

  `VK_MEM_USE` at a vertex, index, uniform, storage or indirect binding of a span `vk_mem_resolve` returned: the pool's
  mirror is created on first use and sp.buffer becomes it (same offsets); a pool that stays imported keeps its buffer.

  `VK_MEM_PICK` where a copy, fill or update reads or writes a span: the mirror if the pool has one, never a new one.

  `vk_mem_gpu_wrote` after recording a GPU write of [sp.offset, sp.offset + size) into sp.buffer: a mirror's bytes go
  back to guest memory at the end of the submission. No-op for imported buffers.

  `vk_mem_mirror_begin` at the start of a submission's recording, `vk_mem_mirror_end` (records the write-backs; returns
  the commands recorded) after its last barrier.

  `runtime/vk/vk_sync.c` records the copies the mirrors' first accesses queued (`vk_mem_mirror_pending`) into a prologue
  command buffer with `vk_mem_mirror_prologue` and submits it right before each part.
- `vk_mem_guest_twin()`: a VRAM mirror span: the pool's imported buffer (guest memory), same offset; else none.
- `vk_mem_gpu_wrote_shader()`: A shader's storage-buffer write: goes back through the write-back merge
  (`runtime/vk/vk_memory.c`), which keeps the words the GPU did not change.
- `vk_usnap_begin()`: Uniform blocks as of the submit call (`runtime/vk/vk_usnap.c`): `vk_exec_record` copies the
  uniform ranges a submission binds before it records, ubo bindings take the copy (0: not copied, bind memory), GPU
  writes of the replay drop the copies they touch.
- `vk_usnap_take()`: `NX2SARIA_REPLAY_ASYNC`: the submitting thread takes the copy before it queues the submission;
  `vk_usnap_begin` on the replay thread then uses it.
- `vk_exec_usnap_hit()`: Replay chunks count hits locally; their totals are merged at the submission boundary.
- `vk_usnap_map()`: a host copy of the current arena, at the offsets `vk_usnap_find` gives.
- `vk_usnap_bytes()`: the copy of [addr, addr+n) as of the submit, NULL: none.
- `vk_usnap_drop()`: the replay makes the GPU write [addr, addr+n) of guest memory.
- `vk_usnap_gen()`: changes whenever a binding may resolve differently.
- `vk_usnap_read()`: [addr, addr+n) into dst: 1 from this submission's copy, 2 from guest memory where the copy lacks it
  but the bytes are ones the CPU writes (an imported pool) and no GPU write in flight touches them, 0 neither (dst
  untouched).
- `enum { VK_USNAP_FOUND, ... }`: Why `vk_usnap_find` finds nothing for [addr, addr+n) in this submission (diagnostics).
- `vk_usnap_shutdown()`: `vk_exec_draw_shutdown`: frees the arenas.
- `enum { MVPREV_NONE, ... }`: `runtime/vk/vk_mvprev.c`: last frame's copy of the scene's object (c7) and bone (c5)
  blocks, for per-object motion vectors. `vk_mvprev_step`: the distance of the game's other copy of a vertex-stage block
  the snapshot also copies, 0: none. `vk_mvprev_find`: last frame's block of that object (prev, its address; how,
  `MVPREV_*`), 0: none.
- `vk_mvprev_want()`: also keep these binding indices' last-frame copies (bit n: index n); DLAA on and
  `NX2SARIA_DLAA_OBJMV` not 0, as of the last present.
- `vk_mvprev_frame()`: each present, beside `vk_dlaa_frame`.
- `vk_mem_gpa_late()`: --probe-vkmem: `NX2SARIA_GPA_LATE`, the table takes a new mirror up in queue order.
- `vk_mem_note_live()`: Staged mirror uploads (`runtime/vk/vk_memory.c`): the GPU of the submission being recorded reads
  guest memory itself (noted by `vk_mem_note_live`); `vk_mem_take_live` returns that and starts the next submission's
  note.
- `enum { VK_RES_EVENT, ... }`: The submission being recorded hands results to the CPU at its completion
  (`runtime/vk/vk_exec_xfer.c`): `vk_sync_recorded_wait` waits for the newest such submission up to the one it waits
  for.
- `vk_exec_note_results()`: `VK_RES_*`.
- `VkExec`: The early tail (`runtime/vk/vk_exec.c` "the early tail", `NX2SARIA_TAIL_EARLY`). `vk_exec_tail`: the replay
  is recording a submission's tail. `runtime/vk/vk_sync.c`: the tail's command buffer set aside for the write-backs' one
  (hold), then the part with the write-backs handed over with the executed timeline's signal and the tail put behind it
  (release); whether a part can take that order now (ok); the game's wait for a fence (`game_wait`); for a value whose
  tail the GPU still runs, about how long that takes (`tail_pending_ns`, else 0).
- `vk_exec_tail_ub()`: `runtime/vk/vk_exec_xfer.c`.
- `vk_mem_live_swap()`: `runtime/vk/vk_memory.c`: the note of a late read of guest memory, exchanged.
- `vk_mem_gpu_wrote_count()`: `runtime/vk/vk_memory.c`: GPU writes of pool memory noted so far.
- `vk_mem_wb_lazy()`: `runtime/vk/vk_memory.c`: the lazy write-back runs.
- `vk_exec_take_results()`: the kinds noted since the last take.
- `vk_mem_lazy_part_end()`: lazy write-back: a part ends (before its prologue), and it is handed over.
- `vk_mem_lazy_decide()`: once the driver is known (after `vk_caps_driver`): on or off, with one log line.
- `vk_mem_fatal_note()`: the memory mode and the lazy write-back's state, for a fatal error's line.
- `vk_sync_rec_fence()`: replay thread: the timeline value of the submission being recorded.
- `vk_sync_release_held()`: `runtime/vk/vk_sync.c`: submits what `vk_queue_submit` holds back when it is needed for the
  timeline to reach value.
- `vk_mem_merge_cpu_wait()`: `NX2SARIA_MERGE_CPU`: the lists up to value are stored.
- `vk_mem_shadow_sync()`: Shadow storage (`runtime/vk/vk_memory.c`): `runtime/vk/vk_sync.c` calls `vk_mem_shadow_sync`
  right before each submit; the replay notes every GPU write into CPU-visible memory at record time
  (`vk_mem_gpu_pending`) and reports it at the fence with `vk_gpu_written`, which copies the bytes back into guest
  memory before telling the texture cache (`vk_guest_written`). fence: the value of the submission the part belongs to
  (0: none, a probe's own sync), at which what shaders stored through the device address table goes back to guest memory
  (`NX2SARIA_SHADOW_TABLE_STORES`).
- `vk_mem_shadow_table_read()`: Record time: a shader of the guest submission being recorded reads the device address
  table (`runtime/vk/vk_gpa.c`). The pools such a shader reaches are never resolved, so the next `vk_mem_shadow_sync`
  brings the CPU's writes into every shadow the table names, once per guest submission.
- `vk_mem_shadow_storage()`: CPU-visible pools are shadow copies of guest memory, not imports (`NX2SARIA_SHADOW`).
- `vk_mem_shadow_table_stores()`: shadow storage keeps what shaders store through the table
  (`NX2SARIA_SHADOW_TABLE_STORES`).
- `vk_gpu_written_back()`: The same for a GPU write into a VRAM mirror, which went back to guest memory with the
  submission: the texture checks see it, the mirror's next sync does not copy it back up. `vk_mem_mirrored` at record
  time, right after the write's buffer was picked (`VK_MEM_USE` / `VK_MEM_PICK`): 1 when [addr, addr+size) lies in one
  pool whose mirror is on.
- `vk_mem_flush_add_probe()`: --probe-vkexec: the flush range list; 0 or the number of the failed check.
- `vk_mem_mirror_uploaded()`: --probe-vkexec: bytes copied into mirrors since the last summary line.
- `vk_mem_mirror_probe()`: --probe-vkexec: the pool at addr gets its mirror as on a first buffer use; 1 when it has one.
- `vk_mem_dmabuf_caps()`: Guest memory as dma-bufs (`runtime/vk/vk_memory.c` "dma-buf imports"): `pick_device` tells
  whether the device has `VK_KHR_external_memory_fd` and `VK_EXT_external_memory_dma_buf` (before `vk_mem_class_log`);
  `create_device` enables them when the pools are imported that way.
- `vk_mem_dmabuf_probe()`: After `vkCreateDevice`, before `vk_mem_lazy_decide`: a page of guest memory imported as the
  pools will be; any refusal turns the dma-buf imports off for the run (shadow storage then). bda: buffers get device
  addresses.
- `vk_mem_class_log()`: once.
- `vk_mem_class_of()`: `VK_MCLS_*` of the pool at addr (NONE when unbacked).
- `vk_mem_census()`: at each present: pool census and traffic, logged at frame 1800 and, with `NX2SARIA_TRACE=perf`,
  every 1800 frames.
- `vk_mem_lost_report()`: a line with the video memory in use and its budget (device lost).
- `vk_mem_device_used_mb()`: device-local memory this process holds (the driver's budget figure), MB; -1 without
  `VK_EXT_memory_budget`. Any thread.
- `vk_mem_stage_log()`: one line after a boot stage: device memory, process commit.
- `vk_mem_rank_log()`: the growth ranking now, "when" leading its minutes of play.
- `vk_mem_exit_log()`: at the process's end: the memory report's growth ranking of the session
  (`runtime/vk/vk_memory.c` `growth_take()`), once it reached play.
- `vk_mem_vram()`: VRAM in use and its budget, summed over the `DEVICE_LOCAL` heaps. exact 1: `VK_EXT_memory_budget`
  (the whole process), 0: this allocator's own allocations against the heap sizes. 0 when unknown.
- `vk_headroom_callbacks()`: VRAM headroom (`runtime/vk/vk_headroom.c`): the VMA callbacks that see new device memory,
  and the thread that allocates the next block of each memory type in use ahead of the render thread.
- `vk_headroom_stop()`: before `vmaDestroyAllocator`.
- `vk_headroom_held()`: bytes the spare blocks' anchors hold.
- `vk_headroom_release()`: a refused allocation: the anchors go, none for 30 s; returns their bytes.
- `vk_mem_give_back()`: A refused allocation: the staging and snapshot buffers kept for reuse and the headroom's spare
  blocks go back to the driver before the caller tries once more (`runtime/vk/vk_memory.c`). Returns the bytes.
- `vk_mem_ballast()`: `NX2SARIA_VRAM_BALLAST_MB` (test): video memory taken for the session (`runtime/vk/vk_memory.c`).
- `vk_vma_block_size()`: VMA's block size on heaps over 1 GiB (`NX2SARIA_VMA_BLOCK_MB`).
- `vk_mem_resolve()`: 1 when all of [addr, addr+size) is backed.
- `vk_mem_lookup()`: `vk_mem_resolve` for a caller that reads guest memory itself or only asks what backs it: with
  shadow storage the pool is not copied in for it (`runtime/vk/vk_memory.c`, `mem_resolve`).
- `vk_mem_gen()`: Changes whenever `vk_mem_resolve` plus `VK_MEM_USE` may give another span for an address: a pool
  added, finalized or mapped, a mirror created, refused or forgotten, a submission started.
- `vk_mem_changed()`: Page write generations over guest memory (`mem_written_pages` plus `vk_guest_written`). Returns 1
  when a page of the range was written after generation `since`, 0 when not, -1 when the range is not write-watched
  (treat as written); `*now` is the generation to pass next time. Generations are 64 bits and do not wrap; 0 is never
  handed out.
- `vk_mem_changed_snap()`: The same answer from per-epoch write-watch snapshots of 2 MiB regions: no system call once
  the regions were queried in this epoch (a guest submission). For checks that may see writes made during the epoch one
  epoch late.
- `vk_mem_snap_stats()`: since the last call.
- `vk_fix_zero()`: Upload batch: one command buffer on `gfx_queue` that texture uploads and texel copies record into.
  begin/end bracket the recording (recursive lock); `vk_staging` (between them) returns a mapped host buffer that lives
  until the batch completes, or, with alloc non-NULL, until the caller's `vmaDestroyBuffer` after `vk_upload_flush(1)`.
  The replay calls `vk_upload_flush(0)` before each submission that may read uploaded images; queue order and the
  batch's closing barrier make the uploads visible to it. Returns the batch sequence number; `wait=1` blocks until it
  completed. `NX2SARIA_FIX_ZERO` (default 1): physical pools are zeroed once at creation and GPU-only images with memory
  of their own are cleared when created; 0 leaves both as the driver hands them out.
- `vk_staging_at()`: A 256-byte-aligned range of the staging arena owned by the open upload batch
  (`runtime/vk/vk_memory.c`): `*buf` at `*off`. Only between `vk_upload_begin` and `vk_upload_end`; a large request gets
  a buffer of its own at offset 0.
- `enum { VK_TS_CREATE, ... }`: Texture work on the render and replay threads, summed per frame
  (`runtime/vk/vk_texture.c` `vk_texstat_frame()`, the "texup:" line of `NX2SARIA_TEXUP_LOG_MS`). `vk_texstat_time` adds
  the time since t0 (`kern_now_ns`) and one event; `vk_texstat_add` adds events and bytes. Other threads are not
  counted. CREATE: new texture entries (image, memory, first transition); UPLOAD: the whole upload, of which STAGE
  (staging range or buffer), COPY (storage into staging: copy, untile, CPU decode) and ASTC (`vk_astc_record`, of which
  SCRATCH: a new scratch buffer); `STG_NEW`: staging buffers made by `vmaCreateBuffer`; DEVMEM: new `VkDeviceMemory` of
  32 MiB or more (VMA blocks, dedicated allocations); FLUSH: `vk_upload_flush` (submit, retire, frees).
- `vk_defer_image()`: Deferred destruction: freed once the upload batches recorded so far and the timeline value last
  noted have completed.

  alloc NULL: `vkDestroyImage` only.
- `vk_defer_free()`: `free(p)` once the GPU is past the submission being recorded, as `vk_defer_image`: a host record
  other threads may still read (a destroyed texture's, `runtime/vk/vk_texture.c` `destroy_tex()`).
- `vk_exec_image_gone()`: texture lock held: the replay's views of image go with it; returns how many.
- `vk_defer_buffer_try()`: As `vk_defer_buffer` without waiting: 0 when another thread holds the upload lock, and
  nothing is deferred.

### ASTC

(`runtime/vk/vk_astc.c`). `NX2SARIA_ASTC` = auto | gpu | native | cpu | check; see `runtime/vk/vk_astc.c`.

- `vk_astc_mode()`: decided once per device (logged); `VK_ASTC_CPU` before `vk_init`.
- `vk_astc_native()`: 1: images of this ASTC format are created as such and get the raw blocks.
- `vk_astc_gpu()`: 1: ASTC images decoded to RGBA8 by the compute decoder (`vk_astc_record`).
- `vk_astc_bc()`: 1: those images are BC3 instead, encoded after the decode (`NX2SARIA_ASTC_BC=1`).
- `vk_astc_bc1()`: 1: and the ones whose blocks decode opaque BC1 (`NX2SARIA_ASTC_BC1`).
- `VkAstcRegion`: One image region decoded from raw blocks: tight rows of 16-byte blocks at byte src of the source
  buffer (slices one after the other), w x h texels and slices deep, written at offset of level / layer. bl: the blocks
  at src are the level's block-linear storage as the game laid it out, block height `2^th` and depth `2^td` GOBs
  (`runtime/gfx/tex_deswizzle.c` `nvn_bl_offset()`), and the decoder reads each block from its place in it (no CPU
  deswizzle); src is 16-byte aligned.
- `vk_astc_record()`: Records into cb, the open upload batch (between `vk_upload_begin` and `vk_upload_end`): src
  (`TRANSFER_SRC`, `src_size` bytes of blocks) goes to a device-local scratch buffer, a dispatch per region decodes it,
  and the texels are copied into img (`VK_IMAGE_LAYOUT_GENERAL`). The caller records its usual barriers around an image
  copy before and after the call. readback (optional, host-visible, `TRANSFER_DST`): also gets the decoded texels, the
  regions one after the other. bc: img is BC3 (`TexVkFormat.astc_bc` 1) or BC1 (2); the texels are encoded to its blocks
  before the copy, and readback is ignored. 1 when recorded. 0 when the decoder is unavailable or cannot take these
  regions, and -1 when its scratch buffer cannot be allocated: nothing is recorded in either case, and after -1 the same
  call can succeed once memory is free.
- `vk_astc_scratch_fail()`: a test's hook: the next n scratch buffer requests fail (`runtime/gfx/tex_probe.c`).
- `vk_texture_upload_retries()`: uploads left to be tried again: their decode had no memory (`runtime/vk/vk_texture.c`
  upload).
- `vk_texture_split_fill_set()`: `runtime/vk/vk_texture.c` "split fill", a test's hooks (`runtime/gfx/tex_probe.c`): the
  mode from here on (`tex_split_fill`: 0, 1 or 2), and how many entries started with a copy of the released entry they
  replaced and how many were filled from their storage.
- `vk_astc_region_bytes()`: source bytes the region reads.
- `vk_astc_stats()`: since start.
- `vk_astc_scratch_bytes()`: the decoder's shared scratch now.
- `vk_astc_scratch_trim()`: each present: a grown scratch idle for 5 s goes.
- `vk_astc_time_enable()`: `NX2SARIA_ASTC_TIME=1`: GPU timestamps around each `vk_astc_record`'s dispatches (logged per
  footprint and the largest one).
- `vk_astc_time_get()`: totals so far.
- `vk_astc_time_summary()`: logs the totals.
- `vk_astc_release()`: `runtime/vk/vk_device.c`, before the device goes: frees the decoder's objects.
- `vk_mem_note_submit()`: `runtime/vk/vk_sync.c`: after each submit that signals `ctx->timeline` with this value.
- `vk_mem_oom_report()`: logs heaps and upload batches after an out-of-memory failure.
- `vk_mem_collect()`: frees what has completed.
- `vk_mem_collect_try()`: the same, unless the upload lock is busy (completion thread).
- `vk_mem_shutdown()`: before `vk_shutdown`: waits idle, frees every memory and texture object.
- `VkTexImage`: Textures. Images are keyed by storage GPU address plus descriptor and stay in `VK_IMAGE_LAYOUT_GENERAL`.
  A mirrored texture (CPU-written pool storage) is uploaded on first use and again when its storage pages were written;
  once the GPU wrote it (`vk_texture_gpu_written`) the image is its content. A returned pointer stays valid until the
  next call that may drop textures (release, pool fini); copy what outlives that.
  - `VkFormat format`: after the fallbacks of `tex_vk_format` (ASTC -> RGBA8, RGB -> RGBA).
  - `VkImageViewType view_type`: view of the whole image.
  - `uint64_t addr`: storage GPU address.
  - `uint8_t alpha_one`: X format: sample alpha as 1 through the view swizzle.
  - `uint8_t chan[4]`: `TexVkFormat.chan`: component order of a stand-in format, applied by the view swizzle.
  - `uint8_t bound`: aliases physical-pool memory (physical and virtual pools).
  - `uint8_t block_views`: BC image that renders through uncompressed views of its blocks (`BLOCK_TEXEL_VIEW_`
    COMPATIBLE + `EXTENDED_USAGE`: only those views may use `COLOR_ATTACHMENT`).
  - `uint8_t swz[4]`: NVN texture swizzle selectors (`nvnTextureBuilderSetSwizzle`); a view swizzle replaces them.
  - `uint8_t fb_usage`: created with `ATTACHMENT_FEEDBACK_LOOP` usage (`VK_FEEDBACK_LAYOUT`, attachable images).
  - `uint8_t fb_learned`: a draw sampled it while it was a written attachment of the open pass: its passes attach it in
    `ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT` (`runtime/vk/vk_exec.c` `vk_exec_pass_begin()`).
  - `uint8_t scale`: 1, or `NX2SARIA_SHADOW_SCALE` for a shadow map made larger than the game's
    (`runtime/vk/vk_texture.c` `shadow_scale_of()`): width and height above are the larger ones, the game's regions are
    multiplied by it.
  - `uint8_t skip`: texture detail (`runtime/vk/vk_texture.c` `detail_skip()`): the game's top mip levels the image was
    made without. Image level i holds the game's level i + skip, and width, height and levels above are the image's, so
    the game's level skip is their base. 0 everywhere else.
  - `uint8_t gone`: destroyed (`runtime/vk/vk_texture.c` `destroy_tex()`, texture lock held): the record stays readable
    until the GPU is past the submission being recorded (`vk_defer_free`), as the replay and DLSS keep pointers to
    render targets across a command buffer; no view of it is cached.
- `vk_texture_flat3d_locked()`: A 2D texture a shader samples as a 3D one: its one-slice 3D copy, refreshed when its
  content changed (`runtime/vk/vk_texture.c`); NULL when it does not qualify. Texture lock held.
- `vk_hdr_present_source()`: The window texture the backend presents (`runtime/vk/vk_backend.c`): `*image` and `*format`
  become the float picture the game copied into it, when the last draw into it was that copy and the swapchain is HDR10.
- `vk_texture_get()`: created and made current; NULL for BUFFER or unmapped formats.
- `vk_texture_by_id()`: A texture slot; a slot whose texture was finalized follows the live texture at the same storage
  (`hle_nvn.c`).
- `vk_texture_lock()`: The replay looks up many slots per draw: it takes the texture lock once around them
  (`vk_texture_lock`) and uses the `_locked` lookups; `vk_texture_slots_gen` changes whenever a slot may resolve to
  another image or view.
- `vk_texture_deferred()`: The replay workers (`runtime/vk/vk_exec.c` "replay workers"): a chunk's lookup that would
  make or take back a texture entry before the chunk's turn returns nothing and says so here; the chunk replays the op
  again in turn (`vk_texture_deferred_take` clears it). `vk_exec_turn_due`: the calling thread records a chunk whose
  earlier chunks may not be done yet; `vk_exec_turn` waits until they are (no-op elsewhere).
- `vk_texture_deferred_stats()`: since the last call, by what the lookup found: none, retired, released.
- `vk_exec_in_chunk()`: the calling thread records a chunk of a parallel group.
- `vk_exec_shared()`: `vk_exec_shared`: other threads may record now (a chunk, or the submitting thread past a group,
  `NX2SARIA_REPLAY_PIPE`), so the shared tables lock; `vk_exec_behind`: the calling thread is that submitting thread;
  `vk_exec_hooks_wait`: then waits until the group's first chunk ran the pass hooks.
- `vk_exec_par()`: The replay thread, while no group of the replay workers is out (`runtime/vk/vk_exec.c` "side work"):
  `fn(arg`, lo, hi) over items [0, n) in slices of grain, on the workers and the calling thread at once; returns when
  every slice is done. Without workers, or for one slice, the caller runs it alone.
- `vk_exec_hook_turn()`: A pass hook (`vk_contact_pass_begin`, `vk_dlaa_pass`) about to change what draws read or to
  record: the submitting thread past a group waits for the group first; a group's hooks run in its first chunk, in turn
  already.
- `vk_exec_stream_pos()`: Where the calling thread records in the order of the stream as the replay workers cut it
  (later records larger).
- `vk_exec_epoch()`: The epoch of a uniform snapshot drop the calling thread makes (write) or of the drops it sees
  (`runtime/vk/vk_usnap.c`).
- `vk_sampler_point_by_id_locked()`: The slot's sampler with nearest filtering and no anisotropy: a depth texture read
  without a compare lands on one texel on the Switch although the game binds a linear sampler. `VK_NULL_HANDLE` for an
  empty slot.
- `vk_sampler_compare_by_id_locked()`: The slot's sampler with its compare on, for a depth-compare read
  (`runtime/vk/vk_texture.c`, at the definition); the slot's own sampler when it compares already. `VK_NULL_HANDLE` for
  an empty slot.
- `vk_texture_detail_auto()`: Texture detail (`runtime/vk/vk_texture.c`): what `texture_detail` = auto picks on this
  device, 0 full, 1 reduced, 2 low (the budget is read at the first call). For the menu's row.
- `vk_texture_detail_name()`: the detail in effect: full, reduced or low; a fatal error's line.
- `vk_sampler_detail_by_id_locked()`: The slot's sampler for a texture made without its top `skip` mip levels
  (`VkTexImage.skip`): its LOD clamp counts the game's levels, so it moves down by skip. The slot's own sampler when its
  clamp is open or skip is 0.
- `vk_texture_exact_by_id_locked()`: Slot id resolved to a texture made without its top levels, and a shader binding
  that reads texels by position or asks for the size wants it: the texture is made again with every level and stays so
  (`runtime/vk/vk_texture.c` "texture detail"). Returns what the slot resolves to now, as `vk_texture_by_id_locked`
  does.
- `vk_texture_create_budget_locked()`: slot lookups create at most n entries (mapped storage only); -1 any.
- `vk_texture_slot_why_locked()`: For the flight recorder: what slot id's own texture is now, as bits (1 the slot is
  finalized, 2 an entry of its own descriptor exists, 4 that entry is released, 8 superseded, 16 overwritten, 32
  uploaded, 64 the storage is readable, 128 held out by disuse, 256 CPU-written, 512 GPU-owned, 1024 retiring), with the
  slot's storage and size.
- `vk_texture_pool_shown_locked()`: The pool's next lookup is of a slot whose entry in the set is a texture's view now
  (1) or a stand-in (0): a slot on screen whose texture is not made is made by that lookup beside the creation budget
  (`runtime/vk/vk_texture.c` `slot_resolve()`).
- `vk_texture_pool_make_ns_take()`: ns the pool's lookups spent making and first uploading entries since the last call.
- `vk_texture_create_deferred_locked()`: a lookup since the last call left a creation to the budget; one past the
  highest texture slot registered so far (or more).
- `VK_POOL_TEX`: Bindless pools (`runtime/vk/vk_exec_draw.c`, "texture and sampler pools"): set 2 of every pipeline
  layout. Shaders that index the texture or sampler pool with handles from memory have their pool arrays moved there
  (`runtime/vk/vk_pipeline.c` `spirv_pool_rebind()`); `VK_POOL_KINDS` image bindings, one per view kind, then the
  sampler pool. `VK_POOL_TEX` covers the game's whole texture pool: Ocarina makes one of 33068 descriptors
  (`nvnTexturePoolInitialize`), and with 32768 the translator's clamp sent every handle from 32768 up to slot 32767, so
  the textures the game numbered there (some ground after an area load) were drawn with whatever slot 32767 held, often
  an empty image: see-through ground until the area was loaded again. 16 kinds x 40960 = 655,360 sampled images in set
  2, under NVIDIA's 1,048,576 update-after-bind limit with room for the bound slots;
  `runtime/vk/vk_exec_draw.c` `pool_layout_make()` logs the device's limits and says which one a failed layout passed.
  `vk_texture_register` logs a slot past the pool.

  as the translator's Accessor.TexturePoolLength.
- `VK_POOL_TEX_CARRIED`: the length before: carried translations still declare it.
- `VK_POOL_SAMP`: Accessor.SamplerPoolLength.
- `VK_POOL_KINDS`: kind = depth * 8 + dim * 2 + arrayed, dim 1D 2D 3D CUBE.
- `vk_pool_kinds_used`: bit k: a pipeline was made with a kind-k pool array.
- `VK_POOL_MARK_BINDING`: Texture pool marks (`runtime/vk/vk_pipeline.c` `spirv_pool_mark()`,
  `runtime/vk/vk_exec_draw.c` "pool marks"): set 2, this binding, a uint per pool slot that shaders set to 1 when they
  index the slot.
- `VK_POOL_MARK_OVERFLOW`: The marks buffer's word past the spare: set by a shader whose ring of noted slots overflowed
  (`runtime/vk/vk_pipeline.c` `spirv_pool_mark()`), so that the read treats every slot as sampled
  (`runtime/vk/vk_texture.c` `vk_pool_marks_read()`).
- `vk_pool_marks_enabled()`: `NX2SARIA_TEX_DISUSE`: modules get the marks.
- `vk_pool_marks_missed`: modules indexing the pool that could not be marked.
- `vk_pool_marks_read()`: `runtime/vk/vk_texture.c`: slots marked since the last read.
- `VK_POOL_SETS`: The texture pool's ring of descriptor sets (`runtime/vk/vk_exec_draw.c` `pool_take_set()`): a
  submission's first pool use takes the next set and brings it up to date. `vk_pool_takes` counts the takes whose sync
  left no slot behind (`runtime/vk/vk_exec_draw.c` `pool_sync()`); once it has moved on by more than `VK_POOL_SETS`
  since a pool entry changed, every set holds the change (`runtime/vk/vk_texture.c` `retire_sweep()`). It may count
  fewer takes than there were, which only delays that.

  4 before: with the game no longer held for the GPU after a frame's first submission, the replay waited for a set's GPU
  work at 15% of the switches.
- `vk_texture_pool_ctx()`: Texture lock held: 1 while the texture pool refresh looks its slots up
  (`runtime/vk/vk_exec_draw.c` `pool_sync_entries()`), 0 after. Those lookups leave out a texture evicted by disuse, and
  are not counted as the texture being used.
- `vk_pool_set_layout()`: the pools' layout (made on first use).
- `vk_empty_set_layout()`: set 1 of layouts without one.
- `vk_tonemap_record()`: The upscaler stand-in (`runtime/vk/vk_tonemap.c`): src (sampled, sw x sh) scaled to dw x dh and
  tonemapped into its own RGBA16F image, which it returns for the caller to blit into the target; `VK_NULL_HANDLE` when
  it cannot. edge: 1 lets it scale with the edge-adaptive filter (`NX2SARIA_STANDIN_FILTER`), 0 keeps the linear one
  (another upscaler's picture replaces this one). The caller binds its own compute pipeline again afterwards.
- `vk_tonemap_forget()`: A source image of the stand-in goes (`runtime/vk/vk_exec_draw.c` `fallback_scratch()`, or a
  game image's destroy through `vk_exec_image_gone`): its views go through the deferred destruction, before another
  image can take its handle. Any thread.
- `vk_texture_slot_token()`: What `vk_texture_by_id(id)` returns can change only when this does: the slot is registered
  again, or a texture entry at its storage address is created, dropped, released or taken back into use. Unlocked; read
  it before the lookup it guards (a lookup may create an entry). 0 when the slot was never registered: nothing is kept
  under 0.
- `vk_texture_bucket_stats()`: diagnosis for the pool log (`runtime/vk/vk_texture.c`).
- `vk_texture_slot_khash_locked()`: `runtime/vk/vk_texture.c`: descriptor hash of a GPU-written slot, 0 otherwise.
- `vk_texture_entry_gen()`: `runtime/vk/vk_texture.c`: bumped by every entry change; registrations that changed
  nothing (`vk_texture_register`), for the pool log.
- `vk_texture_dirty_slots_locked()`: Slots whose token may have changed since `*pos` (0 at first; then the value these
  calls leave): fn is called for each (some more than once) and `*pos` advanced. 0 when too many changes came since: the
  caller looks at every slot. `*pos` from `vk_texture_dirty_pos_locked` after such a walk. Texture lock held. gfn, when
  given, gets each slot group of a bucket in place of fn for its members.
- `VkSlotGroup`: Slot groups (`runtime/vk/vk_texture.c`): the GPU-written slots of one address bucket with one
  descriptor hash kh, which resolve alike. head the first member, `vk_texture_group_next_locked` the next (-1: the end),
  n how many; stamp and fmask the texture pool's (`runtime/vk/vk_exec_draw.c` `pool_dirty_group()`: 1 when every member
  holds the same views, of the kinds fmask; a slot that joins goes to the tail, refilled on its own), set 0 by
  `vk_texture_groups_unstamp_locked`. Texture lock held.
- `vk_texture_pool_note_locked()`: The bindless pool's entries (`runtime/vk/vk_texture.c`): after a pool fill of slot
  id, notes the entry it resolved to (1; 0: none) and gives its number for `vk_texture_pool_hold_locked` in `*ent` (0:
  none); a submission's freshness check of every noted entry (returns how many). Texture lock held.
- `vk_texture_pool_hold_locked()`: A pool slot that held the entry old (0: none) now holds ent: the entries slots hold
  get the use stamps a lookup leaves (`runtime/vk/vk_texture.c` touch) from the check by list. Texture lock held.
- `vk_texture_slot_retry_locked()`: A slot whose last lookup found no entry, looked up again: 1 it resolves now; 0 still
  none, its storage may become readable; -1 none until its token changes (a CPU slot whose texture was released after
  its registration, no storage).
- `vk_mem_gen_lock_shared()`: `runtime/vk/vk_memory.c`, for the pool's freshness check: the generation table's lock,
  shared; with it held, 1 when every 2 MiB region of [addr, addr+size) has this epoch's write-watch snapshot and no page
  stamped after since (nothing in it can have changed), 0 when a closer look is due (`vk_mem_changed_snap`).
- `vk_mem_unchanged_why_locked()`: The same check's reason when it fails: 1 a region without this epoch's snapshot yet,
  2 one untracked, 3 one written since, 4 nothing to check (no size, beyond the regions); 0 unchanged.
- `vk_mem_gen_lock()`: The lock exclusively, for the snapshot of the regions of [addr, addr+size) that have none in
  epoch yet (the number of write-watch queries it took).
- `vk_mem_snap_note_locked()`: The same snapshot for many ranges, with the lock held exclusively: note each range, then
  one call queries the noted regions that have no snapshot in epoch, one query per run of adjacent regions (the number
  of queries). `*left`: 1 when it left regions without one, for `vk_mem_snap_range_locked`.
- `vk_sampler_dlss_bias()`: texture LOD bias added for DLSS upscaling (material samplers).
- `vk_sampler_aniso_live()`: each present: `NX2SARIA_ANISO` after a settings change (tiled samplers).
- `vk_sampler_gen()`: changes whenever a sampler slot is registered.
- `vk_texture_gpu_written()`: rendered or copied into by the GPU: no re-upload from storage.
- `vk_texture_at_locked()`: The live image of the given format and img's size at img's storage address, NULL when there
  is none. Texture lock held.
- `vk_texture_storage_bound_locked()`: bound as a storage image in the recording submission.
- `vk_texture_last_write()`: timeline value of the newest submission recorded to write it; 0: none.
- `vk_texture_texel_span()`: bytes ReadTexels spans; 0 for a bad region.
- `vk_texture_small_dirty()`: a small mirrored target holds GPU texels its storage lacks.
- `vk_texture_write_back_small()`: writes those rendered by work up to fence back to storage.
- `vk_texture_debug()`: `vk_dump`.
- `vk_texture_slot_swz()`: `vk_dump`: swizzle of the descriptor in slot id.
- `vk_texture_read_image()`:
  - `uint64_t img)`: always from the image (GPU copy, waited for); 0 on failure.
- `vk_texture_cpu_stale()`: a GPU-written mirrored texture there has stale storage.
- `vk_texture_dirty_fence()`: newest recorded writer of such a texture; 0: none.
- `vk_texture_write_back()`: writes those images back into their storage (waits).
- `vk_texture_range_gone()`: `vk_pool_fini`: drop the images there.
- `VkTexCensus`: Entries now, live and released (TextureFinalize, kept until `sweep_released`), with the bytes of their
  own memory (images not bound into pool memory). Of the live bytes: those whose content only the GPU made (render
  targets and what the GPU drew or copied into), and the idle ones no lookup reached lately (`runtime/vk/vk_texture.c`
  idle, what the VRAM pressure may evict); of the released bytes: those released too lately for any bound to drop them
  (`stale_after`).
- `vk_texture_cpu_ranges()`: The storage ranges [lo, hi) of the textures whose content the CPU provides, sorted by lo,
  as pairs in a malloc'd array (`*n` pairs); NULL when there are none.
- `vk_texture_host_room()`: `runtime/vk/vk_texture.c`: a pool's import found the host heaps full.
- `vk_mem_tex_pools()`: `runtime/vk/vk_memory.c` "texture storage pools", every few presents.
- `VkTexHost`: The texture cache's host memory for the memory report (`runtime/vk/vk_memory.c` `system_log()`), in
  bytes: its entries and tables, and the CPU's decode and write-back temporaries in use now and at most at once since
  the last call (which resets that).
- `vk_texture_census_log()`: `runtime/vk/vk_memory.c` `vram_log()`: the live entries' memory by kind and format.
- `vk_texture_reuse_log()`: `runtime/vk/vk_memory.c` `owner_log()`: released textures taken back, made again.
- `VkBlockRow`: The holes in VMA's blocks that hold textures (`runtime/vk/vk_texture.c` "compaction",
  `runtime/vk/vk_tex_compact.h`), for `runtime/vk/vk_memory.c` `holes_log()`. A `VkBlockRow` is an allocation of another
  kind that the caller knows to lie in one of VMA's blocks (a VRAM mirror): the block's device memory handle and size,
  the allocation's size and memory type.
- `VK_HOLES_KINDS`: Of the device-local memory type whose blocks hold the most texture memory (type): the blocks that
  hold a texture, their sizes and the textures and rows in them; the textures that could move to another block or be
  dropped (marks 0: the texture pool's marks do not tell which textures are sampled, and none counts as movable); the
  blocks at most half full by what is listed, and those of them that hold nothing but such textures, which are what a
  compaction could give back to the driver. kept: the other half-full blocks in which something listed stays, by what
  stays there (`VK_HOLES_KINDS` kinds, in order: a VRAM mirror, a texture whose content only the GPU made, a texture
  sampled lately, a texture that cannot move for another reason): `kept_with` counts the blocks with that kind in them,
  `kept_only` those where nothing else stays.
- `vk_texture_compact_on()`: Texture compaction (`NX2SARIA_TEX_COMPACT`; `runtime/vk/vk_texture.c` "the pass"): idle
  textures move out of sparsely used blocks so that the blocks go back to the driver. `vk_mem_census` asks at each
  present whether the holes are due a look and, when they are, hands over the VRAM mirrors' rows for the plan; the
  allocator's free callback reports each device memory it gives back (`runtime/vk/vk_headroom.c` `on_free()`; any
  thread).
- `vk_texture_want_room()`: a VRAM mirror that did not fit: idle textures make that much room.
- `vk_texture_commit_low()`: the system's commit was low at the last check (`runtime/vk/vk_texture.c`).
- `vk_mem_vram_live()`: `vk_mem_vram`'s device-local use less the free space inside VMA's blocks
  (`runtime/vk/vk_memory.c`), for the evictions.
- `vk_texture_shutdown()`: `vk_mem_shutdown` calls it.
- `vk_texture_new_submit()`: a queue submit starts: draw bindings check storage again.
- `vk_texture_epoch()`: the write-watch snapshot epoch `vk_texture_new_submit` started.
- `vk_sampler_by_id()`: `VK_NULL_HANDLE` for an empty slot.
- `vk_sampler_for()`: cached by content.
- `vk_sampler_params()`: parameters of a sampler slot; 0 for an empty slot.
- `rt_write_png()`: 8-bit RGBA PNG with stored deflate blocks (`runtime/boot.c`); stops with `rt_fatal` when the file
  cannot be written.

### Submission, synchronisation, completion (`runtime/vk/vk_sync.c`, `runtime/vk/vk_backend.c`)

Fence values are `ctx->timeline` values: each submission signals the next one, in submit order. A value counts as done
once the GPU reached it and the completion thread ran its side effects. Guest or host threads.

- `vk_sync_start()`: `vk_init` (fatal without a device) and the completion thread; idempotent.
- `vk_sync_submit()`: records with `vk_exec_record`, submits; returns the value.
- `vk_sync_submit_own()`: The probes (--probe-vkmem's table parts, the lost-device probe): one submission whose commands
  the caller records itself where the replay would, with the submission lock, the memory prologue and the fence of any
  other.
- `vk_sync_try_locked()`: Runs `fn()` under the submission lock when it is free now and returns 1; returns 0 at once,
  without running it, when another thread holds the lock (`runtime/vk/vk_backend.c` `present_image()`: a frame's
  bookkeeping never waits for another replay).
- `vk_sync_locked()`: Runs `fn()` under the submission lock, waiting for it. For the present thread only, which is not
  one of the game's threads and waits for the lock for its blit's submission anyway
  (`runtime/vk/vk_backend.c` `present_frame_closed()`: a frame's bookkeeping still owed runs before the frame's blit).
- `vk_present_frames_owed()`: `runtime/vk/vk_backend.c` (`runtime/vk/vk_frame_owed.h`): the bookkeeping of the frames
  presented while another thread held the submission lock. `runtime/vk/vk_sync.c` submit calls it right after taking the
  lock, before its replay; a no-op without the back end.
- `vk_sync_reserve()`: asynchronous replay: the value the next queued submission signals.
- `vk_sync_fence_wait()`: 1 when done; 0 on timeout or a value never submitted.
- `vk_sync_exec_wait()`: The executed timeline (`runtime/vk/vk_sync.c`): a submission's own commands are through the
  GPU, its write-backs maybe not yet.
- `vk_sync_wb_split()`: `NX2SARIA_WB_SPLIT`: the write-backs go into a part of their own.
- `vk_sync_exec_stats()`: `NX2SARIA_TRACE=perf` (`runtime/vk/vk_sync.c` "the executed timeline"): what a wait on that
  timeline left behind since the last call, whether the completion thread's callback running now belongs to a value
  whose waiter went on already, and the completion thread's time by callback.
- `vk_sync_cb_value()`: the value whose side effect the calling thread is running; 0: not in one.
- `vk_exec_late_stats()`: `runtime/vk/vk_exec_xfer.c`: such callbacks since the last call.
- `vk_exec_clear_stats()`: `runtime/vk/vk_exec_xfer.c` `clear_small()`: the small buffer clears since the last call and
  the counter records they covered, as a clause for a periodic log line. Replay thread.
- `vk_sync_split_exec()`: `vk_sync_split` for the part before a submission's write-backs.
- `vk_mem_mirror_wb_pending()`: `runtime/vk/vk_memory.c`: `vk_mem_mirror_end` has write-backs to record.
- `vk_sync_fence_done()`: highest done value.
- `vk_sync_issued()`: newest submitted value (the one being recorded is this + 1).
- `vk_sync_finish()`: every submitted value done.
- `vk_sync_shutdown()`: finish, stop the completion thread, free the command buffers.
- `vk_exec_record()`: The op-stream replay (`runtime/vk/vk_exec.c`): records the command handles of one submission into
  cb, which the caller has begun and ends. fence is the value this submission will signal. Returns the number of
  commands recorded into the command buffer it holds at the end (0: that one stays empty and is not submitted). A weak
  `runtime/vk/vk_sync.c` version accepts empty streams only.
- `vk_sync_split()`: Inside `vk_exec_record`, outside a render pass: ends the command buffer recorded so far, submits it
  (after the upload batch) without a signal, and returns a new begun one for the rest of the submission. The
  submission's timeline value is signalled by its last part, whose signal covers every earlier part in submission order.
- `vk_sync_append_room()`: Inside `vk_exec_record`, for the replay workers (`runtime/vk/vk_exec.c`):
  `vk_sync_append_later` ends the command buffer recorded so far, puts it into the part followed by n places for command
  buffers the workers record (`*at` the first), and returns a new begun one for the rest of the part;
  `vk_sync_append_fill` puts the workers' command buffers into their places, before the part ends; `vk_sync_append_room`
  says whether the part has room for n more. A worker claims and begins a command buffer of its own pool for the
  submission being recorded with `vk_sync_worker_begin(w)` and ends it with `vk_sync_worker_end`, on its own thread.
- `vk_sync_pre()`: The part's pre-pass command buffer: submitted right before the part being recorded (after its mirror
  prologue), begun with a full barrier at the first call of a part; `VK_NULL_HANDLE` when it cannot be made. Replay
  thread, `sub_lock` held.
- `vk_exec_mesh_flush()`: `runtime/vk/vk_exec_draw.c`: records into the pre-pass command buffer, as it ends, the stages
  of the emulated mesh draws queued for it (`NX2SARIA_MESH_EMU_BATCH`). A weak `runtime/vk/vk_sync.c` version does
  nothing.
- `vk_sync_cut()`: Ends the part being recorded without handing it to the queue (it goes with the next part that does,
  in order) and returns the command buffer the replay continues in; `VK_NULL_HANDLE` when no more parts can wait. Replay
  thread.
- `enum { GTL_PART, ... }`: The GPU timeline (`NX2SARIA_TRACE=perf`, `runtime/vk/vk_sync.c`): a timed piece of GPU work
  of kind `GTL_*`, handed to the queue at `sub_ns` (`plat_now_ns`), its timestamps start and end (ticks of the graphics
  queue). `vk_gtl_log`: the gaps of the pieces noted since the last call, over frames frames, in the log.
- `vk_gpu_parts_log()`: `NX2SARIA_TRACE=perf`: the timed GPU parts that ended in [t0, t1] (`plat_now_ns`), the interval
  of a frame whose render thread waited `fence_ms` for fences (`runtime/diag/perf.c` `perf_present()`), in one line
  (`runtime/vk/vk_sync.c`).
- `vk_sub_in_frame`: replay: the submission being recorded is the frame's n-th (0 the first; `runtime/vk/vk_backend.c`).
- `vk_sub_in_frame_publish()`: `runtime/vk/vk_sync.c` submit, `sub_lock` held, for a submission with commands:
  `vk_sub_in_frame` takes the place in its frame the submitting thread noted for it
  (`runtime/vk/vk_backend.c` `vk_be_submit()`, `replay_main()`).
- `vk_completion_add()`: Runs `fn(arg)` on the completion thread once fence is done on the GPU (event writes, counter
  reports, sync state). Callbacks run in the order they were added, after those of earlier values; a value already done
  runs at once there.
- `vk_completion_add_result()`: The same for a side effect the game's CPU reads (an occlusion count, a counter report,
  an event value): run before the others, and by the wait after a frame's first submission itself
  (`runtime/vk/vk_sync.c` "the executed timeline").
- `vk_completion_add_first()`: As `vk_completion_add`, but before every `vk_completion_add` callback of the values
  reached (in their own order): the merge lists the CPU stores into guest memory (`NX2SARIA_MERGE_CPU`), which the GPU
  events of the same value come after.
- `rdoc_init()`: `gfx_vulkan_backend` (`runtime/vk/vk_backend.c`): presents go to a present thread and run after the
  submissions made before them.

  runtime/rdoc.c: `NX2SARIA_RDOC` debug capture; before `vkCreateInstance`.
- `vk_present_drain()`: returns once every queued present was submitted to `gfx_queue`.
- `vk_replay_async()`: `NX2SARIA_REPLAY_ASYNC` (default 0): submissions replay on the replay thread.
- `vk_program_release()`: `GfxBackend.program_release`: `runtime/vk/vk_pipeline.c`; weak no-op in
  `runtime/vk/vk_backend.c`.
- `vk_backend_shutdown()`: presents, `vk_sync_shutdown`, `vk_mem_shutdown`, `vk_shutdown` in that order.

### Pipelines (`runtime/vk/vk_pipeline.c`)

Binding model (comment block at the top of `runtime/vk/vk_pipeline.c`): one descriptor set, set 0, per pipeline. Binding
0 is the translator's support buffer (a uniform buffer every stage shares); the vertex stage keeps its own bindings, the
fragment stage's follow them (its SPIR-V is renumbered). Set 0 is a push-descriptor set unless the pipeline needs more
than `maxPushDescriptors` (32 here) descriptors; `VkPipeInfo.bindings` says what goes where. No push constants.
Everything a graphics pipeline depends on, as the op stream carries it. Fields covered by dynamic state are read by
`vk_pipeline_set_dynamic` only and left out of the pipeline key.

- `VkPipeState`:
  - `const GfxShader *vs, *fs`: `gfx_shader_get` results; fs NULL: no fragment stage (depth only). vs is the mesh stage
    of a mesh draw (`gfx_shader_stage` `GFX_STAGE_MS`).
  - `const GfxShader *ts`: mesh draws: the task stage, or NULL.
  - `int32_t color_fmt[8]`: `NVNformat` of colour target i (`OP_SET_RT`), 0 none.
  - `int32_t depth_fmt`: `NVNformat` of the depth/stencil target, 0 none.
  - `uint8_t samples`: sample count of the targets; 0 or 1 single-sampled.
  - `uint8_t prim`: `NVN_DRAW_PRIMITIVE_*` of the draw.
  - `uint8_t depth_minus_w`: `nvn_depth_minus_w()`: clip-space depth is -w..w.
  - `uint8_t depth_clamp, raster_discard`: `OP_DEPTH_CLAMP`, `OP_RASTER_DISCARD`.
  - `uint8_t y_flip`: the replay's viewports have negative heights (upper-left origin): the front face is not mirrored.
  - `uint8_t prim_restart`: the draw's indices restart its strip or fan at the all-ones value (`OP_PRIM_RESTART`, set by
    `op_draw_idx`); dynamic state only.
  - `uint8_t feedback`: the fragment shader samples an attachment of the pass in the feedback layout: 1 colour, 2
    depth/stencil (`VK_PIPELINE_CREATE_*_FEEDBACK_LOOP_BIT_EXT`).
  - `blend_st blend[8]`: `OP_BIND_STATE` kind 0, indexed by `blend_st.target`.
  - `color_st color`: kinds 1-5.
  - `uint32_t vattr[16]`: `OP_BIND_VATTR`: `va_st` words; attribute i feeds vertex shader Location i.
  - `vs_st vstream[16]`: `OP_BIND_VSTREAM`.
- `vk_pipe_state_defaults()`: NVN defaults of every state object (shaders and formats left 0).
- `enum { VK_PIPE_SUPPORT, ... }`: `VkPipeBinding.kind` of the support buffer.
- `VK_PIPE_ZERO_BINDING`: vertex binding of shader inputs without an attribute: bind a zeroed buffer of at least 16
  bytes there (stride 0).
- `VkPipeBinding`:
  - `uint16_t binding`: in its set.
  - `uint8_t stage`: `GFX_STAGE_*` that declared it (support buffer: the first stage).
  - `uint8_t kind`: `GFX_RES_*` or `VK_PIPE_SUPPORT`.
  - `const GfxShaderRes *res`: the stage's reflection entry; NULL for the support buffer.
  - `uint8_t set`: descriptor set: 0, or 1 for the fragment stage with independent layouts.
- `VkPipeInfo`:
  - `VkPipeline pipeline`: `VK_NULL_HANDLE` when creation failed.
  - `VkDescriptorSetLayout set_layout`: set 0.
  - `int push`: 1: write set 0 with vkCmdPushDescriptorSetKHR; 0: allocate and bind a set.
  - `uint32_t vs_inputs`: Locations the vertex shader reads.
  - `VkDescriptorSetLayout set_layout1`: Independent layouts (`vk_caps.gpl`): set 0 holds the vertex stage's bindings,
    set 1 the fragment stage's; bindings are ordered by set. One of the two sets at most is a push set. `n_sets` 1:
    everything in set 0 (`NX2SARIA_GPL=0`, compute).
  - `VkDescriptorUpdateTemplate tpl[2]`: Descriptor update template per set (`runtime/vk/vk_pipeline.c` `make_tpl()`):
    reads one `VkDescSlot` per descriptor, in binding order, from the start of the set's descriptors. `VK_NULL_HANDLE`:
    the set is written with `VkWriteDescriptorSet` structs.
  - `int pool`: a stage indexes the texture or sampler pool: bind set 2 (the pools).
  - `int emu`: `NX2SARIA_MESH_EMULATION`: an emulated mesh pipeline (a generated vertex shader and the fragment stage;
    the task and mesh stages are compute pipelines of the same layout, `vk_pipeline_mesh_emu`), and set 0's compute-side
    template.
  - `int8_t gpa`: `runtime/vk/vk_exec_draw.c` `gpa_reader()`: 0 not known yet, 1 no binding of the device address table,
    2 one (its draws read guest memory as the GPU runs).
- `VkDescSlot`: One descriptor of the packed array a descriptor update template reads (`VkPipeInfo.tpl`), 24 bytes for
  every type; the bytes a type leaves unused are zero, so equal descriptors compare equal byte for byte.
- `vk_pipeline_graphics()`: Both return the cached pipeline (created on first use; failures are cached and logged once).
  info: may be NULL. Valid until `vk_pipeline_shutdown`; thread-safe.
- `vk_pipeline_graphics_async()`: The replay's variant: after `vk_pipeline_async_enable` a missing pipeline is built by
  background workers and this returns `VK_NULL_HANDLE` with `*pending` 1 until it is ready (the draw is skipped);
  otherwise as `vk_pipeline_graphics`. `wait_ms` > 0 waits up to that long for a pending pipeline first. Pipelines built
  in a run are recorded to pipeline-keys.bin next to the pipeline cache and prebuilt at the next boot
  (`NX2SARIA_PIPE_PREBUILD=0`: not).
- `vk_pipeline_graphics_key_matches()`: Compare the requested graphics key with the entry that owns info: -1 when no
  entry is present, else 0 or 1.
- `vk_pipeline_building_ms()`: How long the pipeline of s has been building since a draw first missed it, in
  milliseconds; 0 when it is unknown, usable, or no draw has missed it.
- `vk_pipeline_hint()`: The stage libraries of a draw the replay has not reached yet, queued to the workers when no
  pipeline is known for its state (`runtime/vk/vk_pipeline.c` "stage libraries ahead of their draws"). Returns the
  libraries queued.
- `vk_pipeline_mesh_libs()`: 1 when mesh pipelines are linked from stage libraries like vertex pipelines
  (`NX2SARIA_GPL_MESH`; `runtime/vk/vk_pipeline.c` `mesh_lib_init()` says where not): a new one is then usable after a
  link, not only after its whole build.
- `vk_pipeline_async_enable()`: `rt_boot` calls it unless --shot is given; `NX2SARIA_PIPE_SYNC=1` makes it a no-op. Of
  the probes only --probe-prewarm calls it.
- `vk_pipeline_async_on()`: 1 after `vk_pipeline_async_enable`: graphics shaders are translated in the background too.
- `vk_pipeline_prewarm()`: Boot prewarm, before the guest runs (`rt_boot`): loads the shaders pipeline-keys.bin names,
  translating the ones not cached under the current translator from their stored inputs, and builds every recorded
  pipeline, on all cores. No-op without `vk_pipeline_async_enable`, with `NX2SARIA_PIPE_PREBUILD=0` or with
  `NX2SARIA_GPU=null`.
- `vk_pipeline_prewarm_marker_clear()`: Ends the boot prebuild's crash marker (`runtime/vk/vk_pipeline.c`, "boot
  prebuild crash marker"): `boot_wait` calls it when it lets the game start while the prewarm goes on, so a crash in
  play is not taken for one in the boot's builds.
- `vk_pipeline_prewarm_detach()`: `boot_wait` calls it when it lets the game start while the prewarm goes on: the
  builders of the whole warm after a driver change (`runtime/vk/vk_pipeline.c` `bf_decide()`) go on as in play, at idle
  priority and in the background's numbers.
- `vk_pipeline_frame_note()`: The draws of a frame as it is closed (`runtime/vk/vk_backend.c` `frame_end()`): background
  pipeline builds rest while frames in play are heavy (`runtime/vk/vk_pipeline.c` `play_rest()`). Any thread; called in
  replay order.
- `VkPrewarmProgress`:
  - `int phase`: 0 starting (device, key file), 1 shaders, 2 pipelines, 5 done. Only in the whole warm after a driver
    change (`runtime/vk/vk_pipeline.c` `bf_decide()`): 3 compute pipelines, 4 the prepared variants built into the
    driver's cache.
  - `int64_t sh_done, sh_total, sh_translated`: `gfx_shader_prewarm_progress`.
  - `int64_t pipe_done, pipe_total`: pipelines built or failed of those queued; phases 3 and 4: their counts.
- `vk_pipeline_prewarm_progress()`: any thread, while `vk_pipeline_prewarm` runs or after.
- `MeshEmuInfo`: `NX2SARIA_MESH_EMULATION`: the compute pipelines of an emulated mesh pipeline (built with it); 0 when
  it has none.
- `vk_pipeline_set_dynamic()`: Records every dynamic state that `VkPipeState` determines, after binding a graphics
  pipeline: cull mode, front face, topology, depth and stencil tests and ops, depth bias enable, rasterizer discard,
  vertex input, and, where the GPU has them dynamic, depth clamp, polygon mode, blend enables, equations, write masks
  and alpha to coverage. The replay sets the rest: `vkCmdSetViewportWithCount`, `vkCmdSetScissorWithCount`, depth bias,
  blend constants, stencil compare and write masks and references; vertex buffers with `vkCmdBindVertexBuffers2`
  (strides NULL when `vk_pipeline_vertex_input_dynamic()`, else from `VkPipeState.vstream`).
- `vk_pipeline_line_width()`: The line width for a draw of lines with the state s (width in pixels as the game set it; 0
  = 1): recorded when s draws lines and the device has wide lines, whose pipelines take the width as dynamic state.
  Nothing otherwise.
- `VkVtxInput`: Vertex input as vkCmdSetVertexInputEXT takes it (`runtime/vk/vk_pipeline.c` `vertex_input()`).
- `enum { VK_DYN_CORE, ... }`: The dynamic state `vk_pipeline_set_dynamic_delta` last recorded into a command buffer, as
  the values its calls took. have: the groups that still hold what the command buffer has. Zeroed, nothing is known and
  the next call records everything; the owner zeroes it whenever the command buffer begins again or other code records
  pipelines or dynamic state into it (the replay keeps it in `runtime/vk/vk_exec_draw.c`'s B, which
  `vk_exec_draw_new_cb` clears).

  VTX: topology, primitive restart, vertex input.
- `VkDynShadow`:
  - `uint32_t nt`: colour targets the blend arrays cover.
- `vk_pipeline_set_dynamic_delta()`: `vk_pipeline_set_dynamic` recording only what differs from `*sh`, which it then
  updates. mesh: the bound pipeline is a mesh pipeline, which keeps topology, primitive restart and vertex input static,
  so binding it dropped them: they are left out and marked unknown. Returns the number of state calls recorded and adds
  the number left out to `*skipped`.
- `vk_pipeline_vertex_input_dynamic()`: 1: vertex input is `VK_EXT_vertex_input_dynamic_state`.
- `vk_pipeline_answers()`: Translator answers from the bound state: `attr_type` from the vertex attribute formats (vs),
  `frag_out_type` from the colour target formats (fs). Other fields are left as they are; either pointer may be NULL.
- `vk_pipeline_tex_type()`: `NVN_TEXTURE_TARGET_*` -> `GFX_TEX_*` (`GfxShaderAnswers.tex_type`).
- `vk_pipeline_target_wmask()`: colour write mask of target i the pipeline uses.
- `vk_pipeline_topology()`: quads -> triangle list, line loop -> strip: the replay rewrites those draws; polygon -> fan.
- `vk_vertex_format()`: attribute format (supported fallback), `GFX_TYPE_*` in `*type`.
- `vk_pipeline_push_fn()`: after the first `vk_pipeline_*` call.
- `vk_pipeline_push_tpl_fn()`: Descriptor update templates (after the first `vk_pipeline_*` call): NULL when templates
  are off (`NX2SARIA_DESC_TEMPLATE=0`, a device below Vulkan 1.1) or, for the push variant,
  vkCmdPushDescriptorSetWithTemplateKHR is missing.
- `vk_pipeline_set_indep()`: Probe only: 1 selects stage-independent layouts for pipelines looked up from now on, 0 the
  shared set 0; returns the previous mode. 1 needs `vk_caps.gpl` (the independent-sets flag comes with the extension).
- `VkPipeMem`: The memory report's pipeline figures (`runtime/vk/vk_memory.c` `outside_log()`, `system_log()`), at
  report time. Counts of what holds device memory outside VMA: graphics pipelines by kind (`gfx_none`: entries building
  or failed), of those with a pipeline the ones no draw looked up for `idle_min` minutes and the ones none did in this
  run; built stage libraries by kind (vertex input, pre-rasterization, fragment shader, fragment output); compute
  pipelines likewise; pipeline layouts. Host bytes: the pipeline, library and layout tables, the key file's set and
  records, the shared list's records (only until the boot's prebuild is done with them), the pipeline binaries' index.
  cache: the data the driver holds for pipeline-cache.bin `[0]` and the play cache `[1]`, as last asked (`cache_asked`)
  or else as loaded at the boot.
- `vk_pipeline_save()`: writes the `VkPipelineCache` to pipeline-cache.bin next to nx2saria.exe.
- `vk_pipeline_save_exit()`: `vk_pipeline_save` as the process ends: new builds wait while it waits for the running
  ones.
- `vk_pipeline_autosave()`: `vk_pipeline_save` at most every 20 s, when pipelines were built since.
- `vk_pipeline_shutdown()`: waits for background builds, saves, destroys pipelines, layouts and the cache; before
  `vk_shutdown`.
- `vk_pipeline_selftest()`: one graphics and one compute pipeline from census shaders; 0 on success.
- `vk_pipeline_create_foreign()`: Pipelines a library creates on our device without a pipeline cache of its own (FSR
  3.1, `runtime/vk/vk_ffx.cpp`), through the pipeline caches kept across runs; as `vkCreateComputePipelines` (compute 1)
  or `vkCreateGraphicsPipelines` with no cache.
- `VkForeignPipes`: How many it made so far: `from_cache` came without a compile and compiled went into the play cache
  (both only with a play cache, which is what can tell them apart); uncached were created with no cache of ours.
- `#include "vk_rec.h"`: last: its macros route the `vkCmd*` calls of every file that includes `runtime/vk/vk.h`.
- `VkExec`: Select the main scene view for selective animated motion, and decide which of the frame's other skinned
  draws continue a draw of last frame (`runtime/vk/vk_skin_motion.inc` `skin_resolve()`). ex: the submission that
  records the motion vectors, which are made for the camera block cam and tested against the depth image depth of w x h.
  1: the frame has motion draws to record.

## `runtime/vk/vk_backend.c`

The Vulkan graphics back end: `gfx_vulkan_backend` wires the memory and texture layer (`runtime/vk/vk_memory.c`,
`runtime/vk/vk_texture.c`), submission and fences (`runtime/vk/vk_sync.c`) and the present. Every entry starts the
device on first use.

Present: the game texture is resolved on the calling thread and its blit is handed to a present thread, so
`nvnQueuePresentTexture` returns at once (section 5). Queue order does the rest: the submissions that wrote the texture
are already on `gfx_queue`, and every call that may write or free GPU state the blit reads (submit, finish, pool and
texture teardown, texel writes) first waits until the queued blit was submitted (`vk_present_drain`).

Back-edge pacing: the present thread submits the blit at once, then holds the host present (`vkQueuePresentKHR`) until
the frame's display time from the pacer (`runtime/nvn/nvn_api_obj.c`), so present-to-present intervals stay even
whatever the replay took. The blit is already on the queue, so the wait blocks no submission. While a driver's low
latency holds the frames where they start (`runtime/pacing/vk_lowlat.c`), the pacer's display time is the time of the
game's present, and the host present goes out as soon as the blit is submitted.

Asynchronous replay (`NX2SARIA_REPLAY_ASYNC`, default 0; 1 turns it on): `nvnQueueSubmitCommands` and
`nvnQueuePresentTexture` only queue. Each item gets its timeline value at enqueue (`vk_sync_reserve`), in queue order;
the replay thread runs the synchronous submit and present bodies in that order, so every value is signalled by the
submission it was handed out for. The game runs at most one frame ahead: queueing present N waits until the replay
thread handled present N-1. Calls that read or free GPU state that queued work may use drain the queue first
(`replay_drain`), each cause counted.

- `PresentJob`: `handoff_ns`: how long the presenting thread waited to hand this job over (`present_image`), for the
  present lines. owed: the presenting thread left the frame's bookkeeping to the next holder of the submission lock
  (`present_image`, `runtime/vk/vk_frame_owed.h`); the present thread makes sure it ran before the blit
  (`present_frame_closed`).
- `pcv`: job queued, job taken, job submitted.
- `present_frame_closed()`: "a frame's bookkeeping in replay order".

### `wait_until()`

Sleeps on a high-resolution timer to 1 ms before at, then yields until at: the timer alone wakes up to ~0.5 ms late.

### `pace_smooth()`

- `pace_avg_ns`: the present thread's.

`NX2SARIA_PACE_SMOOTH` (default 1; 0 = off): a picture goes out no sooner than seven eighths of the recent interval
after the one before it. The game's frames reach the present thread in uneven steps although each costs about the same:
with the replay on the submitting thread, work held for an event is replayed with the next frame, so pictures came 13 to
31 ms apart around a mean of 22 ms in a village (5th to 95th percentile), which shows as judder at a frame rate that
would look even. Only the early ones wait, a few milliseconds each, so the rate stays what it was; the recent interval
is a moving average over about four pictures of intervals under 100 ms, so the floor follows a scene that gets lighter
within a second. Not with frame generation, which paces its own presents.

### `pace_shown()`

- `if (interval_ns >= 100000000ull) return`: a load or a hitch is not the rate.

### `present_window()`

`NX2SARIA_TRACE=perf`: the present thread's own window of `PW_PRESENTS` presents. The render thread's perf lines time
the game's side and the hand-off; these say where the present thread's time went: the blit and its submission, the wait
for the picture's time (`pace_smooth`), and the present call, which takes the queue lock and in which an overlay with a
frame limiter sleeps. With the presenting thread's wait to hand each picture over, which that time causes.

### `present_main()`

- `unsigned tr = rt_trace`: read once: the stamps and both lines below follow the same flags.
- `if (j.owed) present_frame_closed(j.frame)`: `vk_dlaa_frame`'s state of this frame, before anything reads it.
- `if (flight_on || vk_dump_white) vk_thumbs_frame(j.frame)`: `NX2SARIA_DUMP_WHITE` reads them too.
- `vk_lowlat_present_frame(j.frame)`: the frame's number for the blit's submit and the present's markers.
- `uint64_t f = vk_sync_submit(NULL, 0)`: a timeline value after the blit: deferred destruction waits for the blit too.
- `perfmon_shown(t1)`: the overlay's frame times: the pictures as shown.
- `NX2SARIA_TRACE=present`: one line per present: the host present's interval, the interval of the game's present calls
  (when the host present went out before back-edge pacing), the wait and how far past its target it went; then how long
  the present call took and how long the presenting thread waited to hand this picture over.
- `vk_pipeline_autosave()`: starts the autosave thread once.

### `fh_limit()`

- `THIN_DRAWS`: `NX2SARIA_FRAME_HOLD_MS` (default 120; 0 = off): a frame whose draws were skipped because a shader or
  pipeline was still building (`runtime/vk/vk_exec_draw.c` `pending_draws`) is not shown: the previous picture stays on
  screen until a frame is complete again. Such a frame lacks what was skipped, which showed for a frame or two as a
  black or unlit picture with the interface intact (a lighting pass), a light going out, or ground and walls missing and
  then back. The game runs on meanwhile, so nothing waits. The hold comes from an allowance of this many milliseconds,
  used up by the time held and filled again at an eighth of the clock's rate: a pipeline that takes longer, or a run of
  them on a first visit with nothing recorded, shows its incomplete frames as before and does not freeze the picture.
  Not with DLSS Frame Generation, which is told about every frame. The same hold covers a change of the scene's size (a
  DLSS or FSR 3.1 mode picked in the menu, `runtime/vk/vk_dlaa.c` `vk_dlaa_resized_take()`): the first frames drawn into
  the game's new, empty scene targets come out dark or black, so the previous picture stays for `RESIZE_HOLD` presents
  from the first frame of the new size. That is once per change of a setting and does not use the allowance. It holds
  with DLSS Frame Generation too: a present left out is a longer gap between two rendered frames to it (its constants
  and tags are set at the host present, `runtime/gfx/gfx_present.c`), and a dark frame it is given comes out as several
  generated ones. And it covers an area load (`runtime/vk/vk_exec_draw.c` `vk_exec_pool_loading()`): while the texture
  pool still makes the area's textures, the frames show the ground and the walls as black stand-ins, so the picture
  before the load stays for up to `LOAD_HOLD_NS`, also beside the allowance. And on Ocarina it covers the frame the game
  leaves empty when it opens a screen over the scene (the Conversation Log over a line of dialogue): that frame clears
  the picture and draws a few interface parts, and the screen with its backdrop follows one frame later, so one black
  frame was shown. A frame of fewer than `THIN_DRAWS` draws right after one of `SCENE_DRAWS` or more is not shown; the
  frames after it follow a frame of few draws and are.
- `FH`: `frame_hold`, at the present, is the only writer of FH. The replay asks what is left of the allowance as it
  records (`vk_frame_hold_left`, `runtime/vk/vk_exec_draw.c`), on whatever thread holds the submission lock, which need
  not be the one that presents: so that reader works out the allowance from `limit_ns`, `left_ns`, `last_ns` and holding
  (atomics) and writes nothing. When it refilled FH itself, a refill on the replaying thread beside one at the present
  on another thread counted the same interval twice or not at all. A refill writes `left_ns` and `last_ns` as a pair
  under a sequence word (seq, odd while it writes), so the reader does not pair a new `left_ns` with an old `last_ns`
  either.

`NX2SARIA_FRAME_HOLD_MS`, read once by whichever thread asks first; that one logs it.

### `fh_left_at()`

The allowance at now from the last refill (left at last): the time since used up while frames were held, filled again at
an eighth of the clock's rate while they were not, never above the limit. Full before the first refill.

### `frame_hold_refill()`

`frame_hold` only; returns the limit.

- `__atomic_thread_fence(__ATOMIC_RELEASE)`: a reader that sees either new field also sees the odd sequence.

### `vk_frame_hold_left()`

Whether a frame that skipped draws now would be held (`runtime/vk/vk_exec_draw.c`: a full-screen draw is skipped only
then). Any thread; reads FH only. `left_ns` and `last_ns` come from one refill: a read that overlaps a refill is made
again, and after `FH_READ_TRIES` such reads the last one is used (it can count one interval twice or not at all, for
this one decision), so the reader never waits for the present. holding is stored at the present just after its refill; a
read between the two applies the previous frame's holding to the few microseconds since that refill. The fields are
loaded before the clock, so now is not before last.

- `__atomic_thread_fence(__ATOMIC_ACQUIRE)`: the fields are read before the sequence is read again.

### `frame_hold()`

1: this frame is not shown. skipped: the draws it lacks; resized: its scene is the first of a new size.

- `uint64_t d = vk_exec_frame.draws`: counted up over the run; set back with `NX2SARIA_TRACE=gfx`.
- `if (FH.resize_left)`: beside the allowance: nothing below counts these frames.

### A frame's bookkeeping in replay order (`runtime/vk/vk_frame_owed.h`)

A present closes the frame for the replay (`frame_end`): the trace and conditional-render frame counts
(`vk_exec_frame_log`, whose `vk_exec_cond_frame_end` moves the frame count the conditional draws go by), dynamic
resolution's step, the DLSS state of the frame (`vk_dlaa_frame`: the mode and its LOD bias, the frame's points and the
swap image, the idle releases), the motion tables (`vk_mvsurvey_frame`, `vk_mvprev_frame` swaps its two) and the GPU
pass timing. The replay reads and writes all of it as it records, under the submission lock. With the replay on the
submitting threads the game presents on one thread while another may be replaying, and running it at the present then
reset it in the middle of that replay: a DLSS frame reset halfway (a shake, a frame without the upscaler), the motion
tables swapped halfway (a ghost; with `NX2SARIA_MVPREV_CHECK=1` a write out of bounds), the conditional draws counted
against the next frame. So it runs under the submission lock: at the present when the lock is free, else first thing in
the next submission, on whichever thread makes it (`runtime/vk/vk_sync.c` submit, `vk_present_frames_owed`), or on the
present thread before the frame's blit (`present_frame_closed`), whichever takes the lock first. The blit reads what
`vk_dlaa_frame` closed (the inputs frame generation must not get across an upscaler or frame generation switch), so a
frame's bookkeeping always comes before its blit, as in 0.10.1. The presenting thread never waits for another thread's
replay for it; the present thread may, for a frame still owed, as it waits for the lock for the blit's submission a
moment later anyway. Nothing in `frame_end` may wait for the present thread, submit or flush with a wait: the hand-off,
`vk_texture_get` with its upload flush and `vk_present_drain` stay in `present_image`, outside the lock.
`NX2SARIA_PRESENT_ORDER=0`: at the present, whatever another thread replays, as in 0.10.1.

### `frame_end()`

t: the presented texture, for the trace = gfx line.

- The frame's draws for the play gate of the background pipeline builds
  (`runtime/vk/vk_pipeline.c` `vk_pipeline_frame_note()`): the game's draws the replay recorded for it, the count
  `frame_hold` tells a scene from a thin frame by. Read before `vk_exec_frame_log`, which sets the run's count back with
  `NX2SARIA_TRACE=gfx`.
- `dynres_frame()`: before `vk_dlaa_frame` reads the DLSS mode it may override.

### `present_order_on()`

- `fo_tex`: the newest frames' presented textures, by frame % `FO_TEX` (atomics).
- `fo_owed_n`: presents that left it to a submission; frames a submission caught up (atomics).
- `fo_blit_n`: frames the present thread caught up before their blits, and the time it took with the wait for the lock
  (the present thread's; read by `order_log`: atomics).

Read once; `present_image` runs one at a time.

### `vk_present_frames_owed()`

`runtime/vk/vk_sync.c` submit, `sub_lock` held, before its replay: the frames presented while another thread held the
lock. Their time counts as this thread's replay time (`perf_replay`), as submit's own timing starts after this call: a
DLSS release in `vk_dlaa_frame` (tens of milliseconds) then shows as replay in the frame-time and slow-frame lines, not
unattributed.

### `fo_blit_here()`

The present thread, before it uses frame (`present_main`): `frame_end` of a frame the presenting thread left owed has
run by then. When no submission came between, the present thread runs it itself, under `sub_lock`, waiting for a replay
in progress. Not one of the game's threads, it waits for the same lock for the blit's submission a moment later anyway;
a frame whose bookkeeping ran (the presenting thread got the lock, or a submission came first) takes no lock here.

### `present_image()`

- `order_log()`: after the held present's state (PH).

The present after the frame count: resolves the image and hands the blit to the present thread. queued: when the game
presented; value: the timeline value queued for this present (asynchronous replay), 0 for the next one (synchronous).

- `if (!vram_probe) vram_probe = 1, perfmon_set_vram_game(vk_mem_device_used_mb)`: the overlay's "game" beside VRAM.
- `int owed = 0`: the frame's bookkeeping may still be owed when the present thread takes it (`PresentJob.owed`).
- `vk_sampler_aniso_live()`: `NX2SARIA_ANISO` changed in the menu: the surface textures' samplers take the new level.
- `if (value)`: the queued value is still signalled, in order.
- `vk_hdr_present_source(img, &j.image, &j.format)`: `NX2SARIA_HDR`: the float picture the game copied into it, same
  size.
- `vk_upload_flush(0)`: an upload `vk_texture_get` just recorded goes before the blit.
- One present in waiting at most: the wait for the present thread to take the previous one is the hand-off
  (`PERF_HANDOFF`), timed from under pl, where pending is read.

### Registrations in submit order

The console's GPU reads the texture and sampler pools as it runs a submission, right after the submit, so a draw sees
the slots as they were at about its submit ([maxwell.md](../maxwell.md) 4.9). The replay takes longer than that, and
writes the submission's bindless set as it goes; the set is read when the submission runs. A registration applied at
once while a submission is being recorded (made by another of the game's threads, or by the game ahead of a replay on
its own thread) therefore reaches that submission's draws, the ones recorded before it too: they sample the slot's next
texture, which the game registered for a later frame. Ocarina registers a round of 30 to 60 streamed textures every four
or five frames in a town. So a registration takes effect between submissions, in the order the game made it:

- Replay on the submitting thread: one made while a submission is being recorded (`reg_replaying`, set by
  `runtime/vk/vk_sync.c` submit around the replay) waits in regq, and so does any made behind it; the submitting thread
  applies them once the submission has gone to the queue (`vk_reg_replay_end`). With no submission being recorded it is
  applied at once.
- Replay on its own thread: every one waits in regq and goes into the replay queue ahead of the next item queued
  (`regs_queue_locked`), so the replay applies it where the game made it among its submits, as it does a release.

A texture finalized while its own registration waits is released at once as before, and once more behind that
registration (`RG_RELEASE`): `vk_texture_release` marks the slots the texture object registered, which the first call
could not see. `NX2SARIA_REG_ORDER=0`: every registration at once, as before.

- `RegItem`:
  - `tex_t t`: `RG_TEX`, `RG_RELEASE`: a copy (the game may reuse the object's memory).
  - `samp_par s`: `RG_SAMP`.
- `regl`: regq and the order of what is applied at once; taken before the texture lock.
- `reg_replaying`: atomic.
- `reg_waited`: under regl: registrations that waited for a submission, and the times some did.

### `reg_push()`

Regl held.

### `reg_add()`

A registration from the game: applied now, or kept for its place among the submissions.

- `else reg_apply(r, 1)`: under regl: in call order with the other threads' registrations.

### `vk_reg_replay_begin()`

`runtime/vk/vk_sync.c` submit, around the replay of a submission of the game's commands, under its submit lock.

### `vk_reg_replay_end()`

The submission has gone to the queue with its set: what waited is applied, before the next submission is recorded.

- `reg_apply(regq, n)`: under regl: a registration made meanwhile comes after these.

### Asynchronous replay

- `ReplayItem`:
  - `const tex_t *tex`: `RI_RELEASE`: a copy the item owns (the game may reuse the object's memory).
  - `RegItem *regs`: `RI_REGS`: n registrations the item owns, in the order the game made them.
- `RQ_CAP`: a frame queues 1 submit and 1 present, and up to a few hundred releases.
- `rcv`: item queued, item taken, item done.
- `dr_calls`: Under rl: calls, calls that waited and the time waited per cause, since the last `replay_log` line and
  over the run.
- `ck_reads`: `NX2SARIA_REPLAY_CHECK`: ReadTexels compared with a read after a full drain (under rl): checked,
  differing, differing although no submission recorded a write to the texture in between, differing with the queue idle
  at the first read.

### `vk_replay_async()`

- Off by default. Where the game waits for the GPU it overlaps the replay with the game threads (a GTX 1650 in one scene
  went from 450-686 frames over 50 ms a minute to 10-13, issue `#88`), the only place a gain was measured. While it was
  the default, a GTX 1660 Ti at the 60 fps cap with the GPU at 26 % reported uneven motion that 0.3.4 did not have
  (`#659`). It adds up to a frame of latency.

### `vk_be_gpu_wait()`

With the replay on its own thread a submission's value covers its replay too, and the wait stays the fence's.

### `vk_be_exec_wait()`

The submit wait (`runtime/nvn/nvn_api_core.c` `NX2SARIA_SUBMIT_FINISH`): the GPU's only where the submission reads guest
memory on the GPU (`vk_sync_recorded_wait`).

### `gfx_replay_async()`

The front end's view (`runtime/nvn/nvn_api_core.c`, `runtime/nvn/nvn_api_cmd.c`): replay is asynchronous only on this
back end.

### `gfx_event_signalling()`

The front end's `nvnEventSignal`, before it writes (`runtime/vk/vk_exec_xfer.c` "events the CPU signals").

### `gfx_replayed_submits()`

The newest submit number (`nvn_submit_count`) whose replay is done: recording copies it used may be freed.

### A present that waits for held work (event order, `runtime/vk/vk_exec.c`)

On the console a present is queued behind its queue's work and goes out once the GPU has run the frame, including work
that waited for an event. The replay holds such work (`runtime/vk/vk_exec.c`, "event order across the game's queues")
until the wait holds, and the present went out at once: in Kokiri Forest dialogue (Saria), several presents a second
came with the frame's whole submission held (4 held submissions, nothing drawn since the last present) and showed the
picture from before, a flash of the whole picture; the frame was then drawn after its present, in the present thread's
empty submission, next to the following frame. So with work held at a present, what can go on now goes first, in a
submission of its own (`held_resume`); if work is still held, the present waits and goes out right after the submission
that replays the last of it, at the next present, or after `PH_MAX` submissions at the latest. Only with the replay on
the submitting thread, as the event order. `NX2SARIA_PRESENT_HELD=0`: every present goes out at once. The game presents
from one thread while others submit, so the submission that puts a held present out may be another thread's, while the
presenting thread goes on to its next present. `runtime/pacing/vk_present_held.h` keeps that whole: one lock (PH.lock)
around arming, putting out and every synchronous present while held presents are on, so a held frame goes out once, in
one piece and before the next one. A submitting thread only tries it and never waits; the presenting thread waits for it
only while another thread is putting a held present out (counted, and timed as `PERF_DRAIN`). No path holds it across
`held_resume`, which is a replay; `present_image`'s callees take `sub_lock` only by trying it.

### `ph_lock_waited()`

`perf_begin`'s clock is `plat_now_ns`'s.

### `present_held_on()`

- `ph_ran`: held work run at a present (`held_resume`), on any thread: atomic.
- `ph_logged`: PH.lock: "went out" lines so far.
- `ph_other`: held presents another thread's submission put out (written under PH.lock, atomic).

### `held_resume()`

The held work that can go on now, in a submission of its own (`vk_exec_record` syncs it like any other: `held_sync`).
The present thread first finishes what it was handed: its empty submission replays held work and frees what it replayed
(`vk_exec_record`), which this walk reads. A present `ph_out` handed it a moment ago is such a case.

### `ph_out()`

`runtime/pacing/vk_present_held.h`'s present callback, with PH.lock held: a held present (why `PH_AFTER`, `PH_CAPPED`,
`PH_NEXT`) or one that did not wait (`PH_NONE`) goes to `present_image`.

### `ph_why_before()`

Asked with PH.lock held, after a submission that ran held work first (before) or after this thread's submission: whether
the held present goes out now.

### `ph_hold()`

The presenting thread, PH.lock held: work is still held, so its present waits.

### `submit_sync()`

- `if (n && ph_armed(&PH))`: a present waits: what can go on goes first, then the present, then this submission.
- `vk_exec_gate_wait(handles, n)`: a submission that begins with a wait for the CPU's signal: before its copies.
- The submission stopped at a wait whose signal the CPU gave while it was being replayed (`runtime/vk/vk_exec_xfer.c`
  "events the CPU signals"): it goes on now, in a submission of its own with copies taken after the signal, and that
  submission's value is the one the game's fence stands for.
- `if (n) ph_flush_try(&PH, ph_why_after, ph_out, NULL)`: at once unless a present waits.

### `vk_sub_in_frame_publish()`

- `sub_place_next`: this thread's next submission's place in its frame.

`vk_sub_in_frame` is the replay's: `runtime/vk/vk_sync.c` submit sets it under the submission lock from the place the
submitting thread noted for its submission (`vk_sub_in_frame_publish`), so another thread about to submit cannot change
it under a replay in progress (`runtime/vk/vk_memory.c` `sub_place()` reads it there and on the replay workers).
Submissions without commands of their own (held work, the present thread's blit) leave it as it was.

### `replay_main()`

- `int sub_idx = 0`: submissions since the last present, in queue order.
- `vk_dlssg_submitted(frame)`: the frame's submissions were replayed before its present.

### `wait_counted()`

Rl held.

### `core_away_unlocked()`

A guest thread waiting for the replay gives its virtual core up, as around any blocking host wait (`NVN_BLOCKING`):
`guest_read` (memcpy, pool invalidate), pool and texture finalize run without it, and a drain lasts up to a replay.
Called and returning with rl held; rl is not held while the core is taken back.

### `rq_room()`

Rl held: the replay thread runs, and the queue has room for `room` items (waits while it has not).

- Rl is dropped while the core is taken back, and another thread may fill the queue again meanwhile: check again.

### `regs_queue_locked()`

Rl held, with room for an item (`rq_room`): the registrations made since the last queued item go into the queue, to be
applied before whatever is queued next ("registrations in submit order"). Taken and queued without letting go of rl, so
two threads' lists keep their order.

### `replay_enqueue()`

Queues an item (waits while the queue is full) and returns its value, reserved under rl so values follow queue order.

- `rq_room(2)`: the item, and the registrations made before it.
- `it->value = it->kind == RI_RELEASE ? 0 : vk_sync_reserve()`: a release signals nothing.

### `replay_drain()`

Returns once every queued item was replayed and submitted. Back-end entries only: the replay thread never calls them.

- `if (__atomic_load_n(&regn, __ATOMIC_RELAXED))`: this thread's own registrations are in: replayed too.

### `replay_log()`

What made the game wait for the replay: since the last line (every 1800 game frames), or over the run (at exit).

### `order_log()`

What the game's threads met of each other at the present (synchronous replay): presents whose frame bookkeeping went to
the next submission because another thread held the submission lock, frames a submission caught up, frames the present
thread caught up before their blits (`present_frame_closed`) and how long that took it, the wait for the lock included,
held presents another thread's submission put out (`ph_out`), and the presenting thread's waits for PH.lock while one
went out (`runtime/pacing/vk_present_held.h` `ph_lock()`). total: the run, at exit (`gfx_exit_log`); else
`NX2SARIA_TRACE=perf` every 600 presents (`present_image`), the window since the last line, logged only when a count
moved.

- `static uint64_t last[7], last_frame`: `present_image`'s (one at a time): the counts at the last line.

### `gfx_exit_log()`

The run total, once: the game exits through `prof_write` (`runtime/boot.c` `_exit`, the window close), probes through
`vk_backend_shutdown`.

### `vk_be_present()`

- `perf_present(frame)`: before `vk_exec_frame_log`, which resets the frame's exec counts the flight recorder reads.
- `if (present_held_on())`: `present_held`.
- `vk_present_drain()`: the present thread's empty submission may run held work: done before it is read.
- `if (ph_armed(&PH)) held_resume(), ph_flush_next(&PH, ph_out, NULL)`: the last present still waits: out now.
- `ph_present(&PH, &j, ph_hold, ph_out, NULL)`: waits for the held work, or goes out now.
- `present_image(t, crop, at_ns, queued, frame, 0)`: only this thread presents: no lock.
- Frame bound: the replay thread handled the previous present before this one is queued.

### `vk_program_release()`

`runtime/vk/vk_pipeline.c` drops a released program's pipelines here.

### `vk_be_texture_register()`

Both take effect between submissions ("registrations in submit order").

### `vk_be_texture_release()`

Asynchronous: the release is queued behind the submissions before it, so their replay still resolves the texture's slots
to its image (a released slot follows another live texture at the storage), and the ones after it see it released, the
order of the synchronous replay. `vk_texture_release` reads only the object's key fields: a copy.

- Its own registration still waits for the submission being replayed: released again behind it, which marks the slot
  that registration fills ("registrations in submit order").

### `vk_be_submit()`

Asynchronous: the handles (`nvn_submit_prepare_async` ran under the front end's submit lock) are copied and queued; the
value returned is the one the replay will signal.

- `vk_lowlat_submit_start()`: a frame's first submission: its simulation is over (nothing while low latency is off).
- `sub_place_next = __atomic_load_n(&nvn_submits_in_frame, __ATOMIC_RELAXED)`: counted up after the submit returns.
- `vk_usnap_take(handles, n)`: the uniform blocks as they are now: the game writes on once this returns.

### `vk_be_read_texels()`

ReadTexels with the replay queued: waits for the newest submission recorded to write the texture (replayed and
completed), not for the whole queue. The readback copy goes through the upload batch on the same queue, after every
submission the replay has submitted. Submissions still queued or being recorded run after the copy, as on hardware when
the GPU has not reached them yet: the game only sees their texels after waiting on a sync, and every sync value covers
replay and GPU, so a read the game synchronised sees them. `NX2SARIA_REPLAY_CHECK` compares with a full drain.

- `vk_sync_fence_wait(f, UINT64_MAX)`: counts as a fence wait in perf.
- `memcpy(again, dst, n)`: bytes between the rows stay the caller's.

### `vk_be_guest_read()`

A CPU read of GPU-written mirrored storage (memcpy, pool invalidate). With the replay queued it waits for the newest
submission recorded to write the textures there, not for the whole queue: the write-back copy then runs after their
renders, and clears `gpu_dirty` only for content that is on the queue. Submissions not replayed yet render after the
copy, as on hardware when the GPU has not reached them; they mark the texture dirty again for a later read.

### `synced_held_count()`

After a fence wait: the value is done, so its replay is too; a later submission the replay records meanwhile marks its
targets with a later fence, which `write_back_small` leaves alone. No drain. The count of such waits that came back
while event-ordered work was still held (`runtime/vk/vk_exec.c`): a submission stopped at an event wait returns its
value with its remainder held for a later replay, so the game may take a wait for that value as the whole submission
done and reuse what the held part still reads. How often that happens decides whether the value should be signalled only
after the remainder (audit finding F-2-05). The counts are atomic adds without a lock: the game's threads wait at the
same time, and each line is logged by the thread whose add reached it. A wait for value 0 is left out: the window
acquire's sync stands for no submission (`runtime/nvn/nvn_api_obj.c` `nvnWindowAcquireTexture` gives it fence 0), and
such waits were half of the lines in two sessions of 2026-11-13.

### `vk_be_frame_start()`

A frame of the game starts. DLSS Frame Generation's low latency sleeps at the acquire (`runtime/vk/vk_dlssg.c`); the
direct path (`runtime/pacing/vk_lowlat.c`) at the acquire or the pad read, whichever the game makes first in the frame.
At most one of the two is in effect in a run, and each returns at once while it is not.

### `vk_be_frame_paced()`

The period `nvnQueuePresentTexture`'s pacer keeps (`GfxBackend.frame_paced`). At most one of the two holds the limit in
a run: DLSS Frame Generation's Reflex, which leaves the pacer its ceiling, or a driver's low latency, which leaves it
nothing. Each lets go of the limit when given 0 (a movie plays).

## `runtime/vk/vk_device.c`

Vulkan instance, physical device choice, logical device, VMA, timeline semaphore and the validation messenger. The VMA
implementation is `runtime/vk/vk_vma.cpp` (VMA is `C++` only).

- `vk_dpa`: The loader's `vkGetDeviceProcAddr` until Streamline is on the device (see `runtime/vk/vk.h`).
- `g_gfx_queues`: queues the graphics family offers (`pick_device`).
- `g_nqfam`: the device's queue families (`pick_device`): their flags and queue counts.
- `lost_fence`: The fence `vk_device_lost_poll` asks about. Nothing submits with it, so its status is "not ready" for as
  long as the device lives.
- `MESA_CACHE_SIZE`: Mesa's drivers (RADV on Linux and the Steam Deck) keep a shader cache of their own beside our
  pipeline cache, by default in ~/.cache and up to 1 GB: about 35 MB in a minute of the title screen. A Deck's /home is
  small and the game usually sits on the SD card, so unless `MESA_SHADER_CACHE_DIR` names a folder, it goes to
  mesa-cache beside the exe. That folder is ours alone, so its limit is raised to `MESA_CACHE_SIZE` unless
  `MESA_SHADER_CACHE_MAX_SIZE` is set: the stage libraries the library warm builds in play live only in the driver's
  cache (`runtime/vk/vk_pipeline.c`, "the play cache"), and a preparation of every shader variant does not fit in 1 GB,
  so the rest would be compiled again in play.

NVIDIA's driver keeps the pipelines it compiles in a disk cache of its own, by default one folder shared with every
program (Linux ~/.cache/nvidia, Windows `%LOCALAPPDATA%\NVIDIA\GLCache`). On Linux one process adds at most
`__GL_SHADER_DISK_CACHE_SIZE` to it (1 GiB unless set), and a start that finds more than that in several files deletes
the later ones. nx2saria.exe --prepare-shaders builds the 42,142 variants of game 1.1.1 into it in one process: with the
default limit it stored about 30,700 of them and the rest were compiled again by the first start after it. On Windows
the same preparation stored them all, and a laptop's updates found the cache cold, 17 to 33% of the list still in it,
with 1.6 GB in its folder (launcher.log, 2026-11-21): the driver's own folder gives the game no way to know who else
opens it or with what limit. So unless the environment sets them, the cache goes to nvidia-cache beside the exe
(`runtime/vk/nvidia_cache.h`), which nothing else writes, with a limit of 8 GiB, and on Windows with the cleanup off.
The driver makes GLCache inside that folder but not the folder, so it is made here; pointed at a folder that is missing
or not writable it caches nothing at all, so when that folder cannot be made or written no variable is set and the
driver keeps its own folder and limit. Without NVIDIA's driver (Linux: no /proc/driver/nvidia/version, the test the
AppImage's build makes for DLSS; Windows: no nvapi64.dll in System32) the variables do nothing and the folder would stay
empty, so nothing is done. `nvidia_cache_dir` = 0 leaves all of it to the driver. Every call of `vk_driver_cache_env`
happens before a driver can load, in the game, the preparations and the probes alike, so a start from the launcher, from
Steam or from the exe itself sets the same.

### `folder_error()`

0 when dir takes a new file, else the system's error code. Linux asks the access rights, as it always has; Windows makes
and deletes a file named after the process, since a directory's attributes say nothing of the rights on it and the
launcher's preparation and a game may start together.

- `snprintf(probe, sizeof probe, "%s\\.write-test-%u", dir, (unsigned)plat_process_id())`: another start may be here
  too.
- `plat_fs_unlink(probe)`: one an earlier start with this process id left.

### `nvidia_cache_beside_exe()`

- A restart inherits what this game set; those variables are the game's, not the player's (`runtime/vk/nvidia_cache.h`).
- `int stale = nvc_stale(&plan, ours)`: set by this game before a restart and not wanted now.
- `int refused = 0`: the variables the environment did not take.
- `if (plan.set & ~refused) plat_setenv(NVC_MARK_VAR, mark)`: a restart will know these are the game's.

### `mesa_submit_thread()`

- `submit_thread_why`: `create_device` logs it on a Mesa driver.

Mesa's Vulkan drivers on Linux (RADV, ANV, NVK) keep timeline semaphores as kernel sync objects, which cannot wait for a
value not submitted yet. Their queues therefore submit on the calling thread and use a thread of the queue's own only
for a submission that waits on such a value. Every other `vkQueueSubmit2`, the command stream's checks and the kernel's
ioctl, runs on the replaying thread, once for every part the replay hands to the queue early
(`runtime/vk/vk_exec.c` `maybe_split()`). `MESA_VK_ENABLE_SUBMIT_THREAD=1` sends every submission to the queue's thread:
in Kokiri Forest on an RX 6800 XT (Mesa 26.2) the replay took 13.9 ms a frame with it and 15.0 ms without.
`NX2SARIA_SUBMIT_THREAD` (default 1 on Linux, 0 on Windows) sets it unless the environment already sets it; 0 leaves the
driver's own choice. Drivers that do not read the variable ignore it. `vk_driver_cache_env` calls this before any thread
can load the driver.

### `vk_driver_cache_env()`

A driver may read these variables when it loads, and a thread can load the Vulkan driver before `create_instance`:
`runtime/patches/widescreen.c`'s `wide_init` asks for the monitor's size (fullscreen with a monitor chosen and the
picture at the window's size), and SDL's load of the Vulkan library there lists the instance extensions, which loads the
drivers, while the prewarm thread may be in `create_instance`; setenv beside other threads' getenv is not safe either.
The quality preset's probe (`runtime/cfg/hw_detect.c` `hw_detect()`, from `cfg_start` inside `mem_init`) therefore calls
this first in a game run, main calls it again for a game run whose probe did not run, before any thread of the game
starts, and `create_instance` calls it for the other modes. Every call happens before other threads exist or under
`vk_init`'s lock, so the flag needs no atomics.

### `create_instance()`

- `vk_dlssg_early()`: DLSS Frame Generation: Streamline before any Vulkan call of the process (its slInit asks so).
- DLSS Frame Generation: Streamline's plugins call its naming functions even when they say it is off.
- Armed, not wanted: the menu switches HDR on during the run (`runtime/vk/vk_hdr.c`), and the extensions are fixed here.
- `if (vk_hdr_armed() && instance_ext_present(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))`: HDR10 surface formats.

### `missing_required()`

Name of the first missing required extension, or NULL.

### `find_gfx_family()`

Graphics+compute family that can present to the window.

### `card_usable()`

1 when the game can run on pd: Vulkan 1.3, the required extensions and a graphics+present queue family (the tests
`pick_device` makes of each card it considers).

### `vk_card_prepare_text()`

- `GPUC_OTHER`: `runtime/vk/gpu_choice.h` has no Vulkan header: its `GPUC_*` kinds are VkPhysicalDeviceType's values,
  which `pick_device`, the preset's probe (`runtime/cfg/hw_detect.c`) and the menu's row (ui/menu.cpp) copy into
  `GpucCard.type` unchanged.
- `g_gpu_choice_note`: The fatal error's note on how the card was chosen
  (`runtime/vk/gpu_choice.h` `gpuc_choice_line()`), empty when it was not or the choice gives no cause to say so.
  Written once, by `pick_device`, before the game runs.
- `g_prepare_text`: The card the device was made on and how it was chosen
  (`runtime/vk/gpu_choice.h` `gpuc_prepare_line()`), for the lines the preparation modes print
  (`runtime/vk/vk_pipeline.c`): the preparation builds the driver's cache of this card, and the launcher's log is where
  a player's report of a slow update is read. Written once, by `pick_device`, before anything reads it.

### `cards_usable()`

Which of the discrete cards the game can use (`runtime/vk/gpu_choice.h` `GpucCard.usable`), before the choice that needs
it.

### `pick_device()`

First discrete GPU with Vulkan 1.3, the required extensions and a graphics+present queue (of one GPU listed by two
drivers, the newer driver's listing); `NX2SARIA_GPU_INDEX` (index) or `NX2SARIA_GPU_NAME` (case-insensitive part of the
name) overrides. The order the cards are listed in changes (`runtime/vk/gpu_choice.h`): with both set, a `gpu_index`
that lists a card without the name is dropped and the name decides; a `gpu_index` alone that lists a card that is not a
discrete one while a discrete one the game can use is listed counts as Auto. The shader preparation modes get their
device here too, from the same settings, so they build the cache of the card the game takes.

- `const char *e = cfg_get("NX2SARIA_GPU_INDEX")`: no fixed default: the best device when unset.
- `uint32_t best_driver = 0`: the driver version of the best one so far.
- The cards in the order the loader lists them now: the list for the log, and the names that tell whether a `gpu_index`
  still lists the card it was written for.
- `const int asked = want`: `gpu_index` as the settings give it: the lines below name it.
- `const int from_env = cfg_from_env("NX2SARIA_GPU_INDEX")`: set for this run, on purpose (`runtime/vk/gpu_choice.h`).
- A position alone that lists a card that is not a discrete one gives way to a discrete card the game can use
  (`runtime/vk/gpu_choice.h` `gpuc_legacy_alt()`), so which of them the game can use is asked here, and only then.
- `gpuc_setting_line(cards, (int)n, want, name, from_env, line, sizeof line)`: what the settings hold.
- A chosen card of another vendor while DLSS Frame Generation is on: the interposer cannot make its device
  (`vk_dlssg_create_device`), and the settings ask for what only an NVIDIA card runs. Not usable, so the best usable one
  is taken below, as for a card that is gone; where no NVIDIA card is usable either, the pass without a choice takes
  what there is and the device is made without frame generation.
- The same GPU listed again by another installed driver (a laptop with the vendor's processor graphics and its card has
  both drivers list the card): the newer driver's listing. The older one, there to serve the processor graphics, was
  reported to crash in its shader compiler with the card.
- A chosen GPU that is not usable or no longer there (the menu's GPU row, an unplugged card, a driver that lists the
  devices in another order): the best usable one instead, so the game still starts and the choice can be changed.
- A card a `gpu_index` alone chose that is not a discrete one, while a discrete one the game can use is listed: the game
  may be on the wrong card (`runtime/vk/gpu_choice.h`). The line follows "using gpu" below, the short form goes to a
  fatal error's report.
- `if (cards[best].type != GPUC_DISCRETE && want >= 0 && !(name && *name))`: a `gpu_index` alone (`gpuc_choice_line`).
- `for (uint32_t i = 0; i < nq; i++)`: a family that does transfer only (async DMA engine).
- `vk_caps_driver(g_vk.phys)`: before `vk_mem_class_log`, which chooses the pools' storage by driver.
- And by whether the device imports dma-bufs (`runtime/vk/vk_memory.c` "dma-buf imports").

### `vram_heap_limits()`

Heap limits for VMA where the GPU has more than one device-local heap. An RX 580 (8 GiB, AMD 23.10) shows a 6 GiB heap
and a 2 GiB host-visible one; Windows granted the game 5.1 GiB of the first. VMA, asked for device-local memory, takes
the first heap whatever its budget, and a save load put 5.6 GiB there while the second held 69 MiB: the next submit
failed with `VK_ERROR_OUT_OF_DEVICE_MEMORY`. With a limit VMA counts a heap full at it and places what follows in the
next memory type that allows it (the other device-local heap first), for every allocation: textures, render targets,
VRAM mirrors, pools. The limit is each device-local heap's budget when the device is made (`VK_EXT_memory_budget`, what
other programs leave) less a margin for memory VMA does not see (swapchain, FidelityFX): a tenth, at least 256 MiB.
`NX2SARIA_VRAM_HEAP_LIMIT`: auto, 0 (no limits, the default) or a number of MiB for the largest device-local heap. The
low-end GPU mode set auto up to 0.10.1.

- `long mb = !e || !*e ? 0 : strcmp(e, "auto") ? atol(e) : -1`: unset: no limits.
- `if (mb < 0 && (nlocal < 2 || !g_vk.has_memory_budget)) return 0`: auto: only where another heap can take the rest.
- `if (budget <= margin * 2) continue`: too small to limit.

### `fatal_note()`

One line for a fatal error's report (`runtime/dispatch.h` `rt_fatal_note_add()`): the card, its driver, and the settings
in effect that change how the game's memory behaves. A report often arrives as the end of a log alone, or as a picture
of it, and the lines that say these things are at its start. No lock is taken: the failing thread may hold any. How the
game's task and mesh programs are drawn (`create_device` decides it): natively, through the mesh fallback, the mesh
emulation, or not at all. Every RX 6000 loss whose marks named a pass was in one that starts with a mesh draw.

### `timed_alloc()`

`NX2SARIA_TRACE=perf`: every device memory allocation VMA makes, timed as the "device memory allocations" work mark
(`runtime/diag/perf.c` `mark_text()`). An area's first visit made 78 of them, 4.4 GB, in a frame of 421 ms of which
about 180 ms showed in no mark; `runtime/vk/vk_headroom.c` measured 8-19 ms per 256 MiB block. VMA takes this one
function from the allocator's create info and the others from its static table
(`runtime/vk/vk_vma.cpp` `VMA_STATIC_VULKAN_FUNCTIONS()`). A failed attempt counts too, with its time and 0 bytes: VMA
may then try again with a smaller block, and the failed call held the frame as well, so the mark's count can pass the
number of allocations made.

### `create_device()`

- Pageable video memory (`NX2SARIA_VRAM_PAGEABLE`): past the budget Windows pages device memory out by the priority of
  each allocation, textures (0.3) before render targets, VRAM mirrors and physical pools (1.0):
  `runtime/vk/vk_texture.c` `image_alloc()`, `runtime/vk/vk_memory.c` `mirror_create()` and `alloc_physical()`. VMA
  passes the priority with its flag (below). Without it the texture residency (`runtime/vk/vk_texture.c` "residency")
  could not bring the use down to the budget.
- The translator's SPIR-V declares fragment shader interlock whenever the host reports it
  (`runtime/vk/vk_pipeline.c` `spirv_fixup()`).
- NVN2 task and mesh programs (gameplay geometry) draw through `VK_EXT_mesh_shader`
  (`runtime/vk/vk_exec_draw.c` `op_draw_mesh()`).
- The foliage's far shadow cascades shade one fragment per 2x2 pixels
  (`runtime/vk/vk_exec_draw.c` `mesh_shading_rate()`).
- Guest shaders that use warp-wide operations after divergent code run with their threads reconverged
  (`runtime/vk/vk_pipeline.c` `spirv_reconverge()`).
- Compiled pipelines kept under the runtime's own keys (`runtime/vk/vk_pipeline.c` `pipe_create()`):
  `VK_KHR_pipeline_binary`, which needs `VK_KHR_maintenance5` (core only from Vulkan 1.4; the instance asks for 1.3).
  `NX2SARIA_PIPE_BINARIES=0` leaves both off.
- Feedback loops (`runtime/vk/vk.h` `VkCtx.feedback`): the layout, and barriers inside dynamic rendering.
- `vk_dlaa_device_exts(ep, ne, ext, &n_ext, 40)`: NGX's; the eight slots after it stay for the eight below.
- FSR 3.1 (`runtime/vk/vk_ffx.cpp`): its Vulkan back end looks vkGetBufferMemoryRequirements2KHR up by that name, which
  resolves only with the extension enabled (the core function has no suffix); without it the back end called address 0.
  Always asked for: FSR 3.1 can be switched on in the menu during a run.
- DLSS Frame Generation (`runtime/vk/vk_dlssg.c`): Streamline's plugins look functions of extensions core in Vulkan 1.3
  up by their suffixed names too, which resolve only with the extension itself enabled.
- GPU timestamps on the CPU clock (`runtime/vk/vk_sync.c`): the perf line splits GPU idle time into waiting for a
  submission or not.
- `if (vk_hdr_armed() && has_ext(ep, ne, VK_EXT_HDR_METADATA_EXTENSION_NAME))`: the display's peak (`vk_hdr_metadata`).
- GPU breadcrumbs for a lost device (`runtime/vk/vk_crumbs.c`): AMD's drivers and RADV report it.
- Pass marks on NVIDIA (`runtime/vk/vk_crumbs.c`): checkpoints only with `NX2SARIA_PASS_CRUMBS=1`, since NVIDIA
  documents a cost for the extension. NVIDIA's drivers report it, AMD's do not.
- Guest memory imported as dma-bufs (`runtime/vk/vk_memory.c` "dma-buf imports"; `pick_device` checked the extensions).
- Low latency without frame generation (`runtime/pacing/vk_lowlat.c`): NVIDIA's extension, which needs
  `VK_KHR_present_id` enabled with it, or AMD's, whose calls need its feature. Each only where the driver lists it; a
  driver lists one of them at most. Not on a device the DLSS Frame Generation proxies make (`vk_dlssg_create_device`):
  that path's own low-latency plugin asks for what it needs there, and the direct path stays off for the run. And only
  in a run that starts with `NX2SARIA_LOW_LATENCY` on: with it off the device is made exactly as before the mode
  existed, and a driver that lists one of the two is only remembered (`lists_low_latency`), so the menu can say that
  turning the setting on takes a restart.
- Graphics pipeline library: chained last so it can be dropped again when `vk_caps_gpl` declines it.
- The driver fills the chain with what it supports; the same chain then enables all of it (robustBufferAccess stays on:
  guest shaders index constant buffers freely and out-of-range reads must stay harmless).
- `if (g_vk.has_vrs)`: the rate is set per draw; no rate attachment and no per-primitive rates.
- These two need features that are not enabled (multiview, primitive fragment shading rate).
- Mesh queries for two diagnoses: `NX2SARIA_GPU_PASSES` (`runtime/vk/vk_crumbs.c`) and the dump's primitive counts
  (`runtime/vk/vk_dump.c` `vk_dump_mesh_begin()`), which also need pipeline statistics and host query reset.
- vkGetDeviceFaultInfoEXT needs the feature, not just the extension.
- `mesh_fallback`: auto, 1 (on every device) or 0 (`runtime/vk/vk_mesh_path.h` says when auto takes it, and why on AMD's
  driver for RDNA 1 and 2 and on Intel's; the RADV releases it takes it on, and the `vk: mesh path:` lines this function
  writes for them, are on [vk-pipeline.md](vk-pipeline.md)). Without mesh shaders the mesh emulation
  (`mesh_emulation` = 1) draws them
  instead. `has_mesh_shader` keeps saying what the hardware has: the recorder thread (`runtime/vk/vk_rec.c`) goes by it,
  not by how mesh programs are drawn.
- Says `NX2SARIA_MESH_FALLBACK_BATCH`'s value and whether it is active here (`runtime/vk/vk_exec_draw.c`); `vk_ctx()` is
  not ready yet, so the path and the device's default go as arguments.
- The chain enables what the driver reported: a present carries a frame's number only with `presentId`, and
  vkAntiLagUpdateAMD may be called only with `antiLag` (the extension alone stays enabled and unused without it).
- Frame generation (`runtime/vk/vk_fg.cpp`) presents on a second queue of the graphics family and acquires on a third.
  AMD's swapchain needs a present queue nothing else submits to (FidelityFX: the engine must not use it, or it can
  deadlock); the image acquire queue needs no capability. Where the graphics family has one queue (AMD's Windows driver,
  an RX 580), both come from another family: a queue of a family that can present for the present, and a second queue of
  it, or of any other family, for the acquire (the present queue itself when there is none). Without them
  `vk_fg_create_swapchain` gave up without a word: frame generation set in the menu did nothing.
- `for (int pass = 0; pass < 2 && fg_pf == UINT32_MAX; pass++)`: compute families first, then the others.
- `if (vk_dlssg_dpa())`: Streamline's, so its plugins hook every entry point the replay fetches.
- `vk_mem_dmabuf_probe(g_vk.device, &g_vk.mem_props, vk_gpa_wanted_ctx(&g_vk))`: may turn shadow storage on.
- `vk_mem_lazy_decide()`: the driver is known: the lazy write-back's default depends on it.
- `static const VmaVulkanFunctions vf = {.vkAllocateMemory = timed_alloc}`: the rest from VMA's static table.

### `destroy_all()`

- `g_errors_total`: outlives the device so the probe can print it after `vk_shutdown`.

- `if (g_vk.device) vk_dlaa_release()`: the DLSS features and NGX: the allocator and the device still live.
- `if (g_vk.device) vk_fsr3_release()`: FSR 3.1's own passes and contexts, where they were made.
- `if (g_vk.device) vk_lowlat_release()`: the sleep's semaphore: the device still lives.
- `if (lost_fence) vkDestroyFence(g_vk.device, lost_fence, NULL)`: `vk_device_lost_poll`.

### `device_fault_report()`

The report of a lost device: the breadcrumbs, the pass marks, the video memory and the driver's fault information. Once
per process, on the thread that saw the loss first (`vk_device_lost_check`).

- `n.vendorBinarySize = 0`: the vendor binary is not fetched; its size alone is logged.
- The faulting access lies in the precision-aligned range around the reported address.

### The end of a run whose device is lost (`runtime/vk/vk.h` `vk_device_lost_check()`)

A lost device stays lost: every later call on it fails or, worse, answers as if it had worked, and a driver that is
called on from several threads after the loss can fault inside itself. So the first thread that sees the loss ends the
run, and nothing else touches the device meanwhile: the paths that would read `vk_lost` and return, and a thread that
gets the loss as a result of its own stops here for good. `LOST_EXIT_MS` bounds the end. The first thread writes the
report and waits for a save commit (`runtime/hle/fs_host.c` `fs_save_crash_wait()`, 2 s at most); should it hang on a
lock that a stopped thread holds, the watchdog (arg non-NULL) says so in one line and ends the process with the same
code, and the lines already written are in the log (`rt_fatal_code` wrote them itself). The end is `plat_terminate`:
`ExitProcess` would stop the watchdog too and then run every library's detach, and a driver's detach that hangs on a
lost device would hold the process for good. A thread that stops here otherwise ends the process a little later, should
the watchdog not have started.

### `lost_wait_exit()`

- `rt_log_ending()`: its line reaches the log file past its cap (`runtime/main.c` `log_sink()`).
- `rt_log_sync()`: the queue's locks only tried (bounded): their holder may be the thread that hangs.

### `vk_device_lost_check()`

- `if (plat_xchg32(&vk_lost, 1)) lost_wait_exit(NULL)`: another thread is ending the run: this one stops here.
- `char at[16]`: when the loss was seen, before the report that follows takes its time.

### `vk_selftest()`

- `vk_sync_signal_note(1)`: a host signal of the timeline is noted first (`runtime/vk/vk_sync.c` `timeline_beyond()`).

### Probe-vkdevice (`runtime/host.h`)

### `probe_vkdevice()`

- `fatal_note()`: the line a fatal error or a crash would add to the log, to read it without one.

### `vk_mesh_shader_native()`

The GPU has mesh shaders (`runtime/vk/vk_exec.c` `vk_exec_tail_begin()`: the early tail runs only on such a card).

## `runtime/vk/vk_caps.c`

What the runtime branches on, logged once at device creation, and the fallbacks for GPUs other than the RX 9070.

Fallbacks (each also forced on any GPU by an environment switch, to test it where the GPU does not need it):

- No `shaderFloat64` (`NX2SARIA_VK_NO_FLOAT64=1` clears the feature before `vkCreateDevice`): the translator lowers FP64
  to FP32 ("softfloat64"), so no shader declares Float64.
- `minStorageBufferOffsetAlignment` above 4 (`NX2SARIA_VK_SSBO_ALIGN=<power of two>` raises it): the translator computes
  storage offsets from the guest address rounded down to that alignment (the translator's unaligned storage path), and
  `runtime/vk/vk_exec_draw.c` `ssbo_info()` binds the rounded-down address, so shader writes reach guest memory instead
  of a copy.
- No graphics pipeline library with fast linking (`NX2SARIA_GPL=0` forces it; it also defaults off on AMD's Windows
  driver for RDNA 2 and older, `vk_caps_gpl()` below): `vk_caps.gpl` = 0, the extension stays disabled, and pipelines
  are built whole.
- No 32-lane pin (no `subgroupSizeControl`, 32 outside min..max, or `NX2SARIA_VK_SUBGROUP_PIN=0`): stages run at the
  device's `subgroupSize` (SPIR-V 1.3 without a required size) and the translator is told that size.
  `NX2SARIA_VK_SUBGROUP=<n>` pins n instead of 32 where the driver allows it.

The defaults (Float64, no storage rounding, 32 everywhere) leave the shader cache keys as they were
(`runtime/gfx/gfx_shader.c`).

The file's state: `gpl_lib` and `gpl_fast` are the driver's `graphicsPipelineLibrary` feature and
`graphicsPipelineLibraryFastLinking` property; `gpl_off` is why `vk_caps.gpl` is off, for the boot report, and NULL when
it is on. `summary` (`vk_caps_summary()`) is written once, in `vk_caps_init()`, before any other thread reads it. `gpu`
is the card for the session summary (`runtime/session_summary.h` `vk_caps_gpu()`): stored once in `vk_caps_init()` and
published by `gpu_known`.

### `vk_caps_vec_index_bug()`

For `runtime/gfx/gfx_shader.c` `patch_vec_index()`: AMD's own Windows driver, of either branch, reads uniform vector
components at computed indices wrongly (the translator's HasVectorIndexingBug, which is set for AMD's Windows drivers).

### `vk_caps_driver()`

AMD split RDNA 1 and 2 off into a driver branch of their own; its `driverInfo` reads "26.10.1 (AMD proprietary shader
compiler)" where the RDNA 3 and 4 driver names LLPC. `vk_caps.amd_sc` is AMD's proprietary driver with "shader
compiler" in that text, `vk_caps.amd_llpc` the proprietary driver without it.

### `vk_caps_gpl()`

`NX2SARIA_GPL=0` turns graphics pipeline libraries off and 1 turns them on; unset, they are on wherever the driver has
fast linking, except on AMD's Windows driver for RDNA 2 and older (`vk_caps.amd_sc`: RX 400 to RX 6000). On an RX 6700
XT the game ended with code 71 at every enemy killed, from 0.5 on, and no longer with gpl = 0; an RX 5700 XT showed the
same ending (issue `#418`). Where in the driver it faults is not established: a kill draws its effects for the first
time, which asks for a burst of new pipelines linked from stage libraries, and building them whole avoids it. The price
there: with libraries off a pipeline the game needs for the first time is built whole by a worker while its draw is left
out (`runtime/vk/vk_exec_draw.c`, the skip reason "pipeline still building"), so new things appear some frames late, and
the start waits for every recorded pipeline instead of 20 s at most (`runtime/vk/vk_pipeline.c` `prewarm_run()`). The
same driver had libraries off before, for device losses at boot (issue `#5`), whose cause was GPU writes past 3D BC
images (the section "3D BC render targets" of `runtime/vk/vk_exec.c`); it boots with them since that was fixed, so gpl =
1 is there to try on a newer driver. RDNA 4 under RADV is reported to hang the GPU at the title screen with GPL.

### `vk_caps_init()`

What it tells the translator (`gfx_shader_set_host`), besides the fallbacks of the file's opening:

- **Gather bias.** Gathers (TLD4) pick their four texels with Maxwell's rounding; AMD and Intel round a coordinate that
  sits between two texels the other way, which draws dark lines along object edges in shadow-map filtering
  ([maxwell.md](../maxwell.md) 2.10; seen on AMD). The translator biases gather coordinates by the host's sub-texel
  precision; NVIDIA rounds as Maxwell does, so the default there is 0. `NX2SARIA_GATHER_BIAS=<bits>` forces it (0: off).
- **Zeroed locals.** A register read before any write is an undefined load in SPIR-V, so function-local variables start
  at 0 ([maxwell.md](../maxwell.md) 3.12). Mesa folds undefined values freely; NVIDIA's compiler gives such a read a
  value of another lane of the warp: the game's clustered-light loop reads registers that only some paths write, and on
  an RTX 5090 the Great Fairy Fountain floor took an extra light term in 4x8-pixel tiles (one 32-lane warp each) and
  Link went dark with green speckles. On with every setting: `runtime/vk/vk_dlaa.c` knows the game's FXAA by its zeroed
  SPIR-V hash too, and the shader cache keys (which include this) stay the same whether DLSS is on or off, so shaders
  prepared with one setting serve the other. `NX2SARIA_ZERO_LOCALS=0` turns it off.
- **Device addresses.** Global accesses the translator finds no storage buffer for go through the device address table
  (`runtime/vk/vk_gpa.c` `vk_gpa_wanted_ctx()` says whether this device has it).

The session summary's video memory is the largest device-local heap: the card's own memory.

## `runtime/vk/gpu_choice.h`

The graphics card the settings choose (`gpu_index`, `gpu_name`) and what the log says about the choice:
`runtime/vk/vk_device.c` `pick_device()` and `fatal_note()`, `runtime/cfg/hw_detect.c` `probe_gpu()`,
`runtime/ui/menu.cpp` `row_gpu()`. Header only, and plain enough for C and `C++`, so that `tests/gpu_choice_check.c`
checks the rule and every line without a device.

The order the cards are listed in is not fixed. Vulkan says nothing of it, and on Windows the loader follows the order
of the system's graphics adapters, which the system's graphics settings for the program, a display on one card and the
power source change. A laptop with an RTX 3050 and Intel UHD Graphics listed the Intel graphics first at one start and
the NVIDIA card first at the next, with the game unchanged: `gpu_index` = 1, chosen in the menu while 1 was the NVIDIA
card, then ran the game on the Intel graphics. Those share the system's memory, which on 16 GB leaves them about 7 GB;
the video memory budget was used up at the first save load and the device was lost (exit code 72), at every start that
loaded a save.

So a position is no lasting choice. The menu writes the card's name beside it (`gpu_name`), and when `gpu_index` lists a
card that does not have that name in its own, the name decides (`gpuc_index`).

A `gpu_index` written alone, as every settings file from before the menu kept the name has it, is the weak case: it
names whatever card is listed at that position now, and the game and the shader preparation both went to it, the
preparation filling the Intel graphics' driver cache of the laptop above while the NVIDIA card's stayed cold. When that
card is not a discrete one while a discrete one the game can use is listed, the position now counts as Auto
(`gpuc_legacy_alt`), with a line in the log. Only the menu's choice of a card writes a name, so a card that is not a
discrete one is kept on purpose by choosing it there. A `gpu_index` that the environment sets (`NX2SARIA_GPU_INDEX`) is
set for that run on purpose and goes by its position; the log then warns when it puts the game on a card that is not a
discrete one while a discrete one the game can use is listed (`gpuc_choice_line`). A card picked through `gpu_name` is
the player's own choice and gets no such line. At each start the log also says what the two settings hold, agreeing or
not (`gpuc_setting_line`), and the shader preparation says which card it builds the driver's cache for
(`gpuc_prepare_line`).

The `GPUC_OTHER` to `GPUC_CPU` values are those of `VkPhysicalDeviceType`, so that this header needs no Vulkan header.
A `GpucCard` is one listed card: the name the driver gives it, its type (`GPUC_*`) and `usable`, whether the game can
run on it (Vulkan 1.3, its extensions, a queue that presents). `usable` is read for discrete cards only:
`gpuc_legacy_alt()`, `gpuc_choice_line()` and `gpuc_prepare_line()` look for one the game can use.

### `gpuc_has()`

1 when part is in s, upper and lower case alike: `gpu_name`'s match.

### `gpuc_index()`

The position to go by, for `gpu_index` want (-1: unset) and `gpu_name` name (NULL or empty: unset) over the n cards
listed now: want, unless a name is set and the card at that position does not have it (the cards are listed in another
order than when `gpu_index` was written, or one of them is gone). Then -1, `*dropped` is 1 and the name finds the card.
Nothing is dropped while one of the two is unset.

### `gpuc_legacy_applies()`

1 when a `gpu_index` alone lists a card that is not a discrete one, the part of `gpuc_legacy_alt` that needs no card's
usable: want (-1: unset) over the n cards listed now lists a card that is not a discrete one, no `gpu_name` is set (a
name is the player's own choice of that card), and the index is not the environment's (`from_env`: set for this run, on
purpose). A caller that has to find out which cards the game can use does so only when this is 1.

### `gpuc_legacy_alt()`

The discrete card a `gpu_index` alone gives way to, or -1 when the position stands: the first discrete card the game can
use, when `gpuc_legacy_applies`. Nothing to give way for with Auto, a position past the list, a discrete card at it, a
name, an environment setting, or with no discrete card to use (the card at the position is then the only one that runs
the game).

### `gpuc_resolve()`

The position the pick goes by for the settings want and name (as `gpuc_index` and `gpuc_legacy_alt` take them), the one
call for the device choice (`runtime/vk/vk_device.c` `pick_device()`) and the preset's probe (`runtime/cfg/hw_detect.c`
`probe_gpu()`), so that the preset is chosen for the card the game takes: `gpuc_index`'s result, or -1 when a
`gpu_index` alone gives way to a discrete card. `*dropped` is `gpuc_index`'s, `*alt` `gpuc_legacy_alt`'s (-1: the
position stands); the caller says either in the log.

### `gpuc_shown()`

The card the settings pick as far as the list tells, for the menu's Graphics card row: the one `gpu_index` lists when it
has the name (or no name is set), else the first with the name; -1 for Auto, for a position past the list, for a name
that no card has (the game then takes the best card, as for Auto) and for a `gpu_index` alone that gives way to a
discrete card (`gpuc_legacy_alt`; `from_env`: the index is the environment's).

### `gpuc_setting_line()`

The line at each start that says what the two settings hold over the cards listed now, agreeing or not, with want and
name as the settings file gives them (-1 and NULL or empty: unset), and `from_env` as for `gpuc_legacy_alt`.

### `gpuc_dropped_line()`

The line for a `gpu_index` that `gpuc_index` dropped, with the want it was given: what it listed, and that the name
decides.

### `gpuc_legacy_line()`

The line for a `gpu_index` alone that gives way to Auto (`gpuc_legacy_alt`), with the want it was given and the card alt
it returned: what the position listed, and what to do to run on that card on purpose (the settings file's `gpu_name`
too, for a game that cannot open its menu).


### `gpuc_prepare_line()`

The `GPUC_BY_*` values say how the card the game runs on was chosen, for this line.

The text the shader preparation modes print after "prepare-shaders: card: " ("prepare-pipelines: card: " in that mode;
`runtime/vk/vk_pipeline.c`): the card whose driver cache they build, picked (an index into c) by how, which is a
`GPUC_BY_*` value, with want and name as the settings give them. Said in the launcher's log, where a player's report of
a slow update is read: the preparation's own device lines are among sixty others. Where the card is not a discrete one
while a discrete one the game can use is listed (the player's own choice of the card by its name, or an index in the
environment), it also says that one's driver cache is not the one built.

### `gpuc_choice_line()`

The card the settings picked (want, name: what the pick went by after `gpuc_index`, -1 and NULL for Auto) is not a
discrete card while one the game can use is listed, and it was chosen by a `gpu_index` alone: the first such card is
returned, line gets the warning for the log and note (NULL: none wanted) the short form for a fatal error's report. A
position says little over the cards' order changing from one start to the next. A `gpu_name`, which the menu writes
beside the position, is the player's own choice of that card, so it gets no warning at each start. Returns -1 and
empties both when there is nothing to say: Auto, a `gpu_name` set, a discrete card picked, or no discrete card to use.

## `runtime/vk/vk_exec.c`

The op-stream replay core of the Vulkan back end: walks the command handles of one submission into its command buffer,
keeps render targets and fixed-function state, opens and closes dynamic rendering passes and records clears and
barriers. Draws, binds and dispatches go to `vk_exec_op_draw` (`runtime/vk/vk_exec_draw.c`); copies, counters, events
and conditional rendering to `vk_exec_op_xfer` (`runtime/vk/vk_exec_xfer.c`). Images stay in `VK_IMAGE_LAYOUT_GENERAL`
(`runtime/vk/vk_texture.c`); full memory barriers order passes, clears and whatever those two record between them.

The op loop handles END / BEGIN / JUMP / CALL and the pure-state ops, applies the state defaults, keeps render targets
until they change and skips clears while rendering is disabled. A pass has GENERAL-layout attachments, LOAD/STORE and a
render area from the smallest attachment, with a full barrier before it.

- `x_cur`: The replay state this thread records with now (inside `vk_exec_record`, or a test's `vk_exec_probe_inline`),
  NULL when it records none: `vk_exec_inline_begin` hands out its command buffer.

### Parallel groups

(`NX2SARIA_REPLAY_WORKERS`; "replay workers" below). A chunk: a part of a group's ops that one thread records into a
command buffer of its own, from the replay state the front pass left at its first op.

- `GROUP_CHUNKS`: chunks a group is cut into at most: `RW_MAX` workers and the submitting thread.
- `LIVE_GROUPS`: groups handed out whose command buffers are not in their part yet, at most.
- `Chunk`:
  - `VkExec ex`: the state at its first op, then the state it records with.
  - `const op_hdr *h`: its first op.
  - `uint32_t nops`: the ops it records (JUMPs not counted).
  - `uint32_t cond0, ncond`: its render conditions' decisions in its group's cond.
  - `uint64_t oq`: the occlusion counting at its first op (`vk_exec_xfer_chunk_state`).
  - `uint32_t slot0, nslots`: its block of occlusion query slots (`vk_exec_xfer_slots`).
  - `uint32_t brk`: its ops that are not state or a draw (`vk_exec_brk_now`).
  - `int idx`: in its group: 0 runs the group's pass hooks.
  - `Group *g`: its group.
- `Group`:
  - `uint8_t *cond`: the decisions of the group's render conditions, in stream order.
  - `volatile int32_t hooks`: chunk 0 began its first pass (or ended): the others may start.
  - `volatile int32_t ndone`: its chunks done.
  - `volatile uint64_t seq`: its number: Q.out when it was handed out (`vk_exec_stream_pos`).
  - `uint32_t upd`: the submission's inline uniform updates before it (`vk_exec_upd_top`).
  - `int at`: its chunks' places in the part's list (`vk_sync_append_later`).
  - `uint64_t out_ns`: when it was handed out.
- `GQ`: Group number s is `GQ[s % LIVE_GROUPS]`. Its chunks wait in Q in the order of the stream once it is handed out;
  the workers (and the submitting thread, while it waits) take them in that order.
- `Q`:
  - `Chunk *q[QN]`: chunk number i of those handed out is `q[i % QN]`.
  - `volatile uint64_t put, take`: chunks handed out, taken.
  - `volatile uint64_t done`: every group up to this number has all its chunks done.
  - `uint64_t out, drained`: the submitting thread: groups handed out, and put into their parts.
- `done_lock`: Q.done moves on (`groups_done_advance`).
- `rw_chunk`: the chunk the calling thread records, NULL outside one.
- `replay_counts`: A chunk is written only by the thread recording it. The submitting thread's other work is local until
  the submission ends. Publishing whole batches avoids contention per draw; a concurrent present can report a completed
  batch in a later frame instead of the frame in which the worker counted it.

### `vk_exec_usnap_hit()`

- `else vk_usnap_hits_add(1)`: a probe or an out-of-replay diagnostic has no submission to flush.

- `rw_behind`: The submitting thread records on past groups whose chunks may not all be done yet
  (`NX2SARIA_REPLAY_PIPE`).
- `enum { PW_NEXT, ... }`: Why it waited for them (`RW.pw_*`): room for the next group, an op chunks never record, a
  pass's hooks, an effect that waits for its turn (`vk_exec_turn`), a texture entry, a part split, the submission's end;
  and the waits that found every group done already.
- `rw_op`: in a chunk: the op it records (`vk_exec_mesh_run` reads on from it).
- `rw_ordered`: every earlier chunk of its group is done.
- `tfr_lock`: target frames (`runtime/vk/vk_target_frames.h`) noted by several chunks at once.

### `NX2SARIA_TRACE=gfx` frame counters

- `fw`: textures written this frame.

### Draw runs

(`NX2SARIA_REPLAY_RUNS=1`, diagnostics for recording a submission on several threads) How the draws of a frame fall into
runs that a recording thread of their own could take: a run is the draws between two ops that have to stay in the
replay's order on one thread (dispatches, copies, texture and buffer clears, events, the game's barriers, a held
submission going on, the end of a submission). The ops a run carries that a serial front pass would have to settle in
order before the threads record (counter reports and resets, render conditions, inline uniform updates, render target
changes, attachment clears) are counted apart. Every 600 presented frames one line: the draws per frame, the share of
them in runs of at least 16, 64 and 256 draws, the runs, and the breaks by kind.

- `RR`:
  - `uint32_t draws`: of the run open now.

### `vk_exec_frame_log()`

- A frame boundary in replay order: `runtime/vk/vk_backend.c` `present_image()` runs it under the submission lock, or
  the next submission does (`runtime/vk/vk_frame_owed.h`).
- `vk_exec_phases_frame(frame)`: `NX2SARIA_REPLAY_PHASES` (`runtime/vk/vk_exec_draw.c`).

### `state_reset()`

NVN defaults of the replay state (`nvn_mtl.m`).

### Render targets

### `view_format()`

A view format other than the texture's is used when both map directly and have the same texel size (the image is created
`MUTABLE_FORMAT` for colour formats). blocks: t is a BC texture with `block_views`, and the view an uncompressed format
whose texel is one of its blocks.

### 3D BC render targets

A 3D BC texture has no block views (`runtime/vk/vk_texture.c` `create_tex()`, `NX2SARIA_BLOCK_VIEWS_3D`): on RDNA2 cards
such an image made `BLOCK_TEXEL_VIEW_COMPATIBLE` and `2D_ARRAY_COMPATIBLE` took GPU writes past the end of its memory
(issue `#5`). Draws into it through an uncompressed view of its blocks (agl's BC4 encoder fills 32x32x32 and 64x64x64
volumes and their levels at the logo) go to a stand-in: a 2D array of the blocks of one level in the view's format, one
layer per depth plane. It starts with the level's content, so a pass that writes part of a plane keeps the rest, and
each pass end copies the planes it drew into the level (a copy between a compressed and a size-compatible uncompressed
format).

- `BC_STANDINS`: one per level drawn: 6 volumes of up to 7 levels at the logo.
- `bcs_lock`: the replay adds and reads; `destroy_tex` (any thread) removes.

### `bc_standin()`

The stand-in of level level of a 3D BC image for draws through view format fmt, made on first use; 0 when none can be
made (logged). Replay thread.

- In the upload batch, which goes to the queue before this part: GENERAL, then the texture's blocks.
- `.extent = {lw, lh, s.d}}`: texels of the compressed source.

### `vk_exec_bc_gone()`

`runtime/vk/vk_exec_draw.c` `vk_exec_image_gone()`: the stand-in of a BC image that goes, freed once the GPU is done
with it.

### `bc_copy_back()`

At a pass end: the planes the pass drew into a stand-in go to its texture.

- `.extent = {bcs[i].w, bcs[i].h, t->layers}}`: texels of the uncompressed source.

### `bc_target()`

A draw into a 3D BC image without block views through an uncompressed view of its blocks: the target is the image's
stand-in (`bc_standin`). 1 when o is filled (target or left out), 0 for any other target.

- `o->image = s.img`: not `img->image`: `view_of` makes this command buffer's view of the stand-in.

### `resolve_target()`

- `if (img && !depth_slot && bc_target(t, v, has_v, img, o)) return`: a 3D BC image: its stand-in.
- `int is3d = img && img->type == VK_IMAGE_TYPE_3D && !depth_slot`: slices drawn through 2D views
  (`2D_ARRAY_COMPATIBLE`).
- A replay worker's lookup that needs its turn returns no image (`vk_texture_deferred`), and the op is replayed again in
  turn (`resolve_rt`'s callers): not a target left out, so not logged as one. Logged, the first such lookup read as if
  the game drew into depth or cube targets the replay could not render.
- A 3D level's depth planes take the place of layers (Vulkan views them as the array layers of a 2D array view).
- `if (blocks)`: a BC texture takes draws only through an uncompressed view of its blocks.
- `if (layers > 1)`: an uncompressed view of a BC image has one layer (no maintenance6 here).
- `if (blocks) o->width = (o->width + 3) / 4, o->height = (o->height + 3) / 4`: the view's extent: 4x4 blocks.

### `vk_exec_noatt_dims()`

The size of a pass without attachments: the far corner of viewport 0, else of the scissor, 1..16384.

- NX2Saria draws a fragment shader that only writes a storage buffer into a 1x1.
- viewport; a pass sized by the viewport's width alone puts an offset viewport outside the scissor, and the cave rock.
- Loses its ambient light.

### `resolve_rt()`

Resolves `ex->rt` to this command buffer's images and derives the pass parameters and the pipeline's target formats.

### `view_of()`

The attachment view of a target. A registered texture's view lives as long as its image (`vk_exec_target_view`); any
other image's view, and that of a texture destroyed since the target was resolved (gone: its record is freed only after
this submission, `vk_defer_free`), is made for this command buffer and freed at its fence.

- `if (t->blocks) ci.pNext = &use`: not the BC image's sampling and transfer usages.

### `free_views()`

Completion thread, once the submission that used them is done.

### Target history

(`runtime/vk/vk_target_frames.h`: the table, what recent and steady mean, and the pass verdict).

- `vk_exec_steady_passes`: lookups a steady target alone made per-frame (`vk_exec_targets_recent`).

### `vk_exec_targets_recent()`

`tfr_pass_recent` for the current pass; `vk_exec_steady_passes` counts the lookups that only a steady target made
per-frame.

### `vk_exec_targets_describe()`

One line naming each target of the current pass: format, extent, layer and the two frames it was last rendered in
(`prepare_draw`'s pipeline wait log).

### Passes and barriers

### `barrier_game()`

Full barrier. The images in the feedback layout become `want[]` (pass begin) or none (every other barrier): the others
go back to GENERAL and the new ones leave it, in the same command. `NX2SARIA_BARRIER_GAME` (1; 0: every pass begin and
dispatch a full barrier; ab: in the B blocks): the game's own barriers order its work, as on the Switch. A pass that
samples what an earlier one drew, a dispatch that reads what another dispatch wrote, an indirect draw whose arguments a
dispatch wrote: on the Switch each needs `nvnCommandBufferBarrier` (`ORDER_*` and `INVALIDATE_*` bits) in the game's
command stream, which the replay keeps as a full barrier (`OP_BARRIER`). What the GPU orders by itself there is kept: a
switch between graphics and compute work drains the one before (the 3D and compute classes share the engine). With every
pass begin and every dispatch a full barrier, 2.3 ms of a 9.7 ms GPU frame in Kokiri Forest was barriers. With the
switch:

- A pass begin after an earlier pass and no dispatch since the last full barrier gets an attachment barrier (colour and
  depth/stencil output before the next pass's loads and writes: Vulkan orders attachment access only inside one pass
  instance), after neither nothing;
- A dispatch after other dispatches only gets none;
- A pass after a dispatch, and a dispatch after a pass, get a full barrier, the drain of the switch;
- Either gets a full barrier as before after one the replay itself asked for (`vk_exec_barrier`: its copies, clears,
  fills, uniform updates and stand-in copies call it before their commands, which sets `bar_dirty`), and when
  feedback-loop targets change layout.

Copies, clears and the game's barriers keep their full barriers.

The default is 1 on NVIDIA GPUs and 0 on every other. The game placed its barriers for an NVIDIA GPU, and what it left
out is what such a GPU orders by itself; Vulkan asks for a barrier wherever a pass samples what an earlier pass drew or
a dispatch reads what an earlier one wrote, whether the game recorded one or not. On an RTX 5090 the picture was the
same with the switch on and off (34 screenshots across A/B blocks in Kokiri Forest) and the GPU time fell by 0.5 to 0.6
ms of 5.4 a frame; no other vendor's driver has run it, so there the setting has to be turned on by hand.

### `barrier_to()`

`NX2SARIA_TRACE=perf`: barriers per frame by kind (`vk_exec_barrier_log`).

### `barrier_to_why()`

- `if (why == BR_OTHER) ex->bar_dirty = 1`: also when it falls together with the last one: its commands follow.
- `if (why != BR_OTHER) ex->bar_dirty = 0`: after `BR_OTHER` the replay's own commands follow: the next pass or.
- `ex->bar_passes = ex->bar_dispatches = 0`: dispatch orders after them.

### `vk_exec_pass_begin()`

- `if (vk_texture_deferred()) return`: a chunk's lookup waits for its turn (`chunk_record`): the op is replayed again.
- A parallel group's targets do not change: its pass hooks, which follow the frame pass by pass, run in its first chunk,
  which the others start after (`chunk_record`), and at that chunk's first pass alone: a pass it begins again later may
  come after the hooks of passes further on in the stream (`runtime/vk/vk_pass_hooks.h`).
- The submitting thread past a group: the hooks read what its first chunk's left, and wait for the whole group where
  they change what its draws read (`vk_exec_hook_turn`).
- `vk_dlaa_pass(ex)`: DLAA runs before the bloom chain's first pass (`runtime/vk/vk_dlaa.c`).
- `vk_dlaa_retarget(ex)`: after DLSS ran before the bloom: the passes it moves to the picture's size
  (`runtime/vk/vk_dlaa.c`).
- Learned targets (`VkTexImage.fb_learned`) are attached in the feedback layout.
- `barrier_to_why(ex, want, wasp, nwant, BR_PASS)`: passes, clears, copies and dispatches see each other's results.
- The start mark goes between the barrier and `vkCmdBeginRendering`, its end right after `vkCmdEndRendering`
  (`vk_exec_pass_end`): outside the pass, and without breaking it.
- `VkPassMark *m = ex->mark = rw_chunk ? NULL : vk_pass_mark_begin(ex->cb, VK_MARK_PASS, ex->fence)`: one ring.
- The shaders bound now are the first draw's when it repeats the pipeline state; when it does not,
  `runtime/vk/vk_exec_draw.c` `prepare_draw()` replaces them as it records the new state.

### `vk_exec_pass_end()`

- `vk_exec_frame_log` resets the frame counters at a present (`NX2SARIA_TRACE=gfx`): then only the draws since it count.
- `for (int i = 0; i < ex->pass_ncolor && i < 8; i++)`: stand-ins of 3D BC targets go to their textures.
- The targets are marked written outside `tfr_lock`: `vk_texture_gpu_written` takes the texture lock, and a chunk that
  holds the texture lock can end its pass here (a lookup's alias copy, `vk_exec_inline_begin`) and then wait for
  `tfr_lock`. Taken the other way round, a chunk holding `tfr_lock` and waiting for the texture lock deadlocked with it,
  the other chunks spinning in their turns (the loading screen after Continue, 3 replay workers). `tfr_lock` guards only
  the target history and takes no other lock inside.
- `vk_dlaa_pass_end(ex)`: a jittered pass left its targets' outer rows or columns undrawn.

### `vk_exec_inline_begin()`

- `vk_exec_barrier(ex)`: ends the pass; feedback-layout targets go back to GENERAL.

### `vk_exec_inline_end()`

- `VkExec *ex = x_cur`: set: the caller's `vk_exec_inline_begin` handed out its command buffer.

### `free_inline_buf()`

Completion thread.

### `vk_exec_inline_buffer()`

- `vk_completion_add(x_cur->fence, free_inline_buf, t)`: inside `vk_exec_inline_begin` and `_end`.

### `vk_exec_scissor()`

- `if (vk_dlaa_target_scale(ex, &sx, &sy))`: a pass DLSS moved to the picture's size
  (`runtime/vk/vk_dlaa.c` `vk_dlaa_retarget()`).

### Clears

### `whole_target()`

Outside a pass, a clear of a whole target (no narrower scissor, the target as large as the render area) clears the image
directly instead of opening a pass for it.

- `vkCmdClear*Image` addresses a 3D level whole, not one depth plane: slices of a 3D target clear in a pass; it cannot
  clear a compressed image: views of BC blocks clear in a pass too.

### `clear_attachment()`

- `vk_exec_feedback_war(ex, bit)`: a draw of this pass sampled it: its reads finish before the clear writes.

### `clear_cond_log()`

`NX2SARIA_LOG_RCOND=1`: clears issued under a render condition (the first 400), run or skipped, with their target.

- `if (on == 2 && index < 0 && ex->has_render_cond && ex->depth.width >= 1600)`: full-size depth clears, all of them.

### `clear_color()`

- `if (mask != 0xf)`: `vkCmdClearAttachments` ignores write masks; a masked clear needs a draw
  (`runtime/vk/vk_exec_draw.c`).
- `memcpy(cv.float32, c->rgba, sizeof cv.float32)`: integer targets: the payload's bits as the front end stored them.

### The op loop

### `set_rt()`

- `if (!memcmp(&ex->rt, r, sizeof *r)) return`: the same targets again: the pass goes on.

### Command buffer parts

NVN starts the GPU as soon as commands are submitted; one Vulkan command buffer per submission starts it only after the
whole replay. Between passes, once `NX2SARIA_SPLIT_DRAWS` (64; 0 off) draws and dispatches were recorded since the last
part, the part goes to the queue (`vk_sync_split`) and the replay continues in a new command buffer. Only command-buffer
state is lost there (bindings and dynamic state: `vk_exec_draw_new_cb`, `ex->dirty`); barriers, layouts and queries are
not: a barrier's first scope is everything earlier in submission order, image layouts belong to the image, and occlusion
queries end with their pass. Fences, completion callbacks and reports stay per submission: only the last part signals
the value. The texture pool set a part binds is sealed when the part goes (`vk_exec_pool_split`): later parts write and
bind another, and a part is not split off while no other set is free of this submission. A part also spans at least
`NX2SARIA_SPLIT_US` (1000) of replay time: a split costs about 40 us of the render thread (`vkQueueSubmit2`, a command
buffer, the mirror prologue), and a fight frame of 2,000 draws made 15 parts on the 7900X; a millisecond of recording is
a fraction of a millisecond of GPU work, so the GPU still starts early. With `NX2SARIA_SPLIT_IDLE=1`, neither limit
holds the first part of a submission back while the GPU has run everything submitted: it goes at the first pass boundary
after a draw or dispatch. Each frame the game's thread waits for the GPU to run the frame's first submission
(`runtime/nvn/nvn_api_core.c` `nvnQueueSubmitCommands`, `NX2SARIA_SUBMIT_FINISH`), so the GPU is idle when the next one
replays, and waited for the first 64 draws and 1 ms of it in Kokiri Forest (2026-11-09). Off by default: with it the
foliage of Kokiri Forest flickered (RTX 5090, 2026-11-09), and the parts it cuts that early are the one thing it
changes; why they matter is not known yet. Both limits also split inside a pass (`NX2SARIA_SPLIT_IN_PASS`, on): the pass
ends there and the next draw begins it again in the new part, as after a barrier (attachments load and store; occlusion
queries continue in a new segment). Kokiri Forest's two scene passes of ~500 draws take 2-4 ms each to replay and most
of the frame's GPU time; split only between passes, the GPU waited for each whole pass, idle up to 3.6 ms a frame
(2026-11-09). After the time limit held a part back, the clock is read again only 8 draws later (`split_at`): read after
every command, it was 4% of the replay in Kokiri Forest.

### The early tail

(`NX2SARIA_TAIL_EARLY`, default 1 since 0.10.0; 0 turns it off) Standing in Hyrule Field with FSR 4 (RX 7900 XT,
2026-11-13) a frame's main submission ends with one part of 4.7 ms of GPU time, the temporal upscaler and the interface
drawn after it, and one of 1.4 ms, the write-backs of what the shaders wrote into VRAM mirrors. The game waits for all
of it before it writes the next frame's data (the wait after a frame's first submission, `runtime/nvn/nvn_api_core.c`,
and its own fence waits), about 7 ms in which the replay thread has nothing to record. What the game reads of the
submission is done before the upscaler runs: the occlusion and counter reports, the event values, and the write-backs'
bytes, which only came last. After it the stream holds two timestamp reports, and inline uniform updates for the
interface's draws (64 bytes into one mirrored block, some thirty times), and nothing else the CPU sees. So from the
upscaler on (`runtime/vk/vk_dlaa.c` evaluate) the replay records the tail into a command buffer of its own
(`vk_sync_cut`), without splits or parallel groups. An inline uniform update of a mirrored block in it takes no
write-back: the GPU copies the bytes into the mirror in stream order as ever, and guest memory gets them from the CPU
with the submission's results (`runtime/vk/vk_exec_draw.c` `op_update_ub()`,
`runtime/vk/vk_exec_xfer.c` `vk_exec_tail_ub()`). At the submission's end, when the tail turned out to hand the CPU
nothing but timestamps (`vk_exec_tail_results`), to write no other pool memory (`vk_mem_gpu_wrote_count`) and to bind no
memory the GPU reads from the game's own pages (`vk_mem_live_swap`: a pool without a mirror, a device address table),
the write-backs are recorded into a command buffer that goes to the queue before the tail's, with the executed
timeline's signal (`vk_sync_tail_hold`, `vk_sync_tail_release`): the GPU runs the same commands, the write-backs before
the tail. The mirror copies the tail's first uses queued go with that part too, so they read the game's pages before the
signal. The game's waits then end at that signal and the results the game reads
(`runtime/vk/vk_sync.c` `vk_sync_exec_wait()`, `vk_sync_game_wait()`), and the GPU runs the tail while the game and the
replay are at the next frame. A timestamp reported in the tail is written then with the time the tail is expected to end
at (`runtime/vk/vk_exec_xfer.c` `counter_cb()`), as the game measures the frame's GPU time with it. A tail that fails a
test goes as before, the write-backs after it. Measured there, 150 s runs, each pair in one batch: 63.5 against 49.4 fps
with three replay workers; without them 50.7 against 42.5 standing and 56.0 against 45.9 along the field route, and with
FSR 3.1, whose tail the GPU runs for 1.2 ms where FSR 4's takes 3.5, 51.8 against 47.8 and 54.9 against 52.3. All but a
few of the tails passed the tests there; in area loads and some places a tail hands the CPU results and goes as before.
The pictures matched those without it standing, along the route and through warps to five areas. With the lazy
write-back (NVIDIA's and Intel's Windows default) every tail was refused there until now: its uniform updates were noted
as writes, since their write-backs would run before the copies that make the bytes. They go the early way now too
(`runtime/vk/vk_exec_draw.c` `op_update_ub()`): the bytes reach guest memory at the fence instead of by write-back, and
a range the lazy fetch path has armed stays refused as before. Not yet measured on those cards. Not with shadow storage,
emulated mesh shaders, the mesh fallback (`runtime/vk/vk_mesh_path.h`: never measured with the early tail), a frame dump
or a pass capture, nor inside a chunk or a continuation.

- `TL`:
  - `int on`: the submission's tail is being recorded.
  - `int live0`: `vk_mem_live_swap`'s note from before it.
  - `uint64_t wrote0`: `vk_mem_gpu_wrote_count` when it began.
  - `uint64_t n, early, why[4]`: since the last perf line: tails; those that went early; those refused by results,
    writes, late reads, other.
- `vk_exec_tail_results()`: `runtime/vk/vk_exec_xfer.c`.
- `vk_mesh_shader_native()`: `runtime/vk/vk_device.c`.

### `vk_exec_split_now()`

- `VkCommandBuffer cb = vk_sync_cut()`: not handed to the queue: the texture pool set stays (`vk_exec_pool_split`).

### `split_wanted()`

- The first part only: later parts of the submission run on the GPU, while done and issued stay as they were.

### `maybe_split()`

- `if (!split_n || TL.on) return`: the tail stays one command buffer (the early tail).
- `if (Q.drained < Q.out)`: groups the submitting thread went on past go into the part first, once done.
- `if (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < Q.out) return`: `split_draws` stays: a later op asks again.
- The first part while the GPU is idle: at a pass boundary only, where a split costs no pass restart.
- `if (split_ns && kern_now_ns() - part_t0 < split_ns)`: `split_draws` stays: a later draw asks again.
- `if (ex->in_pass) vk_exec_pass_end(ex), n_pass_splits++`: the next draw begins the pass again, in the new part.

### Event order across the game's queues

Ocarina runs work on a second queue that hands results back and forth with the main one by events: each frame the worker
submits first, its command buffer waits (WaitEvent) for a signal the main queue gives in its submission a few submits
later, does a step, signals, waits for the next point of the main queue's frame, and so on, while the main queue waits
for the worker's signals before it uses the results (Kokiri Forest: about 13 waits a frame, 7 of them reached before
their signal in submission order). On one Vulkan queue in submission order the worker's steps would run before the work
they wait for and read the previous frame's results: Link and the other characters were drawn much too dark, at any time
of day. So a top-level wait whose condition does not hold in replay order (`vk_exec_wait_event`: memory, or the signals
replayed so far) stops its submission there: the rest becomes a continuation of that queue (Cont), and later submissions
of the queue line up behind it. After every replayed signal the continuations whose wait now holds go on, inside the
command buffer being recorded: the open pass ends, a full barrier orders them after what came before (the event's memory
dependency), the queue's own state replaces the running one while they run, and another barrier and the running state
come back after. One that would still wait after `CONT_MAX` submissions goes on regardless, as it did before (counted,
logged). Only with the replay on the submitting thread: the recording copies a continuation reads stay alive there until
the game recycles them, which it does only after the work ran on the console. `NX2SARIA_EVENT_ORDER=0`: every submission
in the order it came.

- `Cont`:
  - `struct Cont *next`: the next submission of the same queue.
  - `uint64_t *h`: private copies of the recordings it has still to walk (`cont_new`), the one being walked.
  - `const op_hdr *at`: the wait it stopped at in `h[i]` (NULL: not started).
  - `uint64_t since`: `vk_exec_submit_seq` when it stopped.
  - `int has_st`: st holds the queue's state (else the running state carries over, as for any submission).
- `evq_peek`: What `vk_exec_held_ready` needs of each queue's first continuation, as a copy
  (`runtime/vk/vk_evo_peek.h`): it runs without the submission lock (`runtime/vk/vk_sync.c` `sub_lock`) while another
  thread may free that continuation (`evo_resume`). Published under that lock by `peek_publish` wherever `evq[s].head`,
  or the head's at or since, changes.
- `evo_on`: knob; inside a continuation.
- `evo_queue`: queue of the stream being walked (0: it cannot stop at a wait).
- `evo_pending`: Continuations held: written under the submission lock (`cont_push`, `evo_resume`), read by any thread
  (`vk_exec_held`).
- `evo_held`: since the last perf line.

### Event order trace (`NX2SARIA_EVO_TRACE=1`, diagnostics)

The order in which a frame's submissions, waits, signals and held parts were replayed, as a string of tokens:
`|<queue>.<n>` a submission of n command buffers on that queue; p it lines up behind held work of its queue; `m<e>` a
wait on event e that holds; `u<e>` one that does not; h the submission stops there; `s<e>=<v>` a replayed signal;
`r<queue>` held work of that queue goes on; w it stops at a wait again; d it is through; F it went on without its
signal; `c<e>=<v>` the CPU signals an event (`nvnEventSignal`, on whatever thread, where the call came between the
others); `L<e>` a wait that holds only by a CPU signal that came after the submission took its copies
(`runtime/vk/vk_exec_xfer.c` "events the CPU signals"). Events and queues are numbered in the order they first appear; a
value of 16 or more is written n, as a counter would make every frame's string its own. Every distinct string is logged
once with its number, and the numbers of the frames are logged sixty to a line, so the frames that were replayed in
another order than usual can be set against the pictures of the flight recorder.

### `cont_new()`

Holds `h[from..n`), stopped at `at` in `h[from]` (NULL: from its start). The recordings are copied: the game records a
command buffer again once the fence it took after the submission is done, and that fence does not wait for a held part
(it signals with the submission that held it).

### `peek_publish()`

Publishes queue s's first continuation for `vk_exec_held_ready` (`evq_peek`): whether one is held, the wait it stopped
at, copied out of its private recording, and when it stopped.

### `cont_push()`

- `__atomic_store_n(&evo_pending, evo_pending + 1, __ATOMIC_RELAXED)`: the only writer: under the submission lock.

### `st_swap()`

Swaps the queue state in ex with buf (`ST_HI` - `ST_LO` bytes); what the swapped-in state names is bound again.

### The gate

A submission that begins with a wait for the CPU's signal (`NX2SARIA_GATE_WAIT_MS`, default 10; 0 off) Ocarina's main
queue begins with a wait for an event the game signals from the CPU once it has written what the frame reads
(`runtime/vk/vk_exec_xfer.c` "events the CPU signals"). The render thread submits that queue without waiting for the
signal, and polls the event afterwards, a millisecond at a time, before it presents. In Hyrule Field the signal came 1
to 2 ms after the submit call in about every other frame: the main thread first waits for the previous frame's fence and
reads back what it needs. The submission then took its copies (the mirror sync and the uniform snapshot, 1.1 to 1.7 ms),
met the wait unmet and was held; the render thread polled, presented, and the present replayed the frame
(`runtime/vk/vk_backend.c` `held_resume()`) under a value the game's fence does not stand for, so the game's next wait
for that fence returned with the GPU still at work and the frame after was replayed inside its submit call again: frames
of 23 and 15 ms in turn where each takes 17. So when a submission would stop at such a wait before it has recorded
anything, the submit call waits for the signal here, before the copies are taken, as the console's GPU waits at the wait
itself: the copies are then taken after the game has finished writing, and the frame is replayed in the call the game's
fence stands for. Only state may precede the wait in the command buffer, and the event must be one the CPU has signalled
before; the wait ends after `NX2SARIA_GATE_WAIT_MS` at the latest, and the submission is then held as before. Since the
last perf line: waits, their time, waits that ran out. `vk_exec_gate_wait` runs on the submitting thread before the
submission lock, so several threads add at once: atomics, and the thread whose add ends the window logs it.

### `gate_first_wait()`

- State the walk only stores: nothing is recorded and nothing reaches the CPU before the wait.
- `default: return NULL`: a draw, a copy, a signal, a call: the walk decides, as before.

### `vk_exec_gate_wait()`

- `if (__atomic_add_fetch(&GW.n, 1, __ATOMIC_RELAXED) == 600)`: one thread takes the window; adds after it count in.

### `vk_exec_held_ready()`

Reads only the published copies (`evq_peek`): `runtime/vk/vk_backend.c` `held_resume()` and `submit_sync()` call this
without the submission lock. A copy that overlapped a publish on every try counts as not ready, as losing that race did
before. The wait is tested on the event's value now (`vk_exec_event_met_now`), not at the copy point of a submission
another thread may be recording: the question is whether held work can go on in a submission of its own, which takes its
copies later. `vk_exec_record`, the one caller under the lock, asks right after its own copy point, where the two agree.

### `held_sync()`

Held work that goes on in a submission without commands of its own (the present thread's empty one after its blit,
`runtime/vk/vk_backend.c` `present_main()`, or the one a waiting present makes, `present_held`) gets what any
submission's recording gets: the VRAM mirror sync before and the write-back after (`vk_mem_mirror_begin/end`), and a
uniform copy (`vk_usnap`). Without them its draws read mirrors that missed the CPU's writes since the last submission,
and its GPU writes never went back to guest memory: the next `vk_mem_mirror_begin` dropped the write-back list.
`NX2SARIA_HELD_SYNC=0`: as before.

### `cont_walk()`

Walks the continuation from where it stopped; 1 when it stopped at another wait (`c->i`, `c->at` updated), 0 when done.

### `evo_resume()`

Runs the held continuations whose wait holds now, in the command buffer being recorded, until none can go on.

- `if (cont_walk(ex, c))`: waits again: the queue's state stays with it.
- `evq[s].head = c->next`: done: the queue's next submission starts with the state this one left.
- `peek_publish(s)`: the next one, or none; the copy names nothing inside c.
- `memcpy((uint8_t *)ex + ST_LO, mine, ST_HI - ST_LO)`: the running stream's state back.

### Looking ahead for pipelines (`NX2SARIA_PIPE_AHEAD`, default 1; 0 off)

A draw whose pipeline is not there yet waits for its fast link or is skipped until a worker has built it, and the next
draw with a new pipeline is only found when the replay reaches it: an area's first frame, or a turn that brings a dozen
new materials into view, paid for them one after another (each wait a few milliseconds with the stage libraries in the
driver's cache, 100 ms and more without), or showed frames with those draws missing while the workers that could have
built them all at once sat idle. The submission is recorded before it is replayed, so what comes after the draw is
known. The first draw of a submission that finds a shader or pipeline missing has the rest read here (`vk_exec_ahead`),
from the op after it to the end of the submission's last command buffer: the scan applies the binds that decide a draw's
shaders and its shader libraries (program, state objects, vertex attributes, textures and images, render targets, depth
clamp) to a copy of the replay state and, at each draw, looks its shaders up as the draw will and names the state to
`vk_pipeline_hint`, which queues the libraries not built yet. Nothing is recorded and no Vulkan object is touched; a
shader not in memory is queued for its disk read or translation by the lookup itself. The draws then find their
libraries built, or building on several workers at once. What the scan does not follow: the op stream past a wait for
another queue's signal (what follows may be held), a change of the texture pool, mesh draws where their pipelines have
no stage libraries (`vk_pipeline_mesh_libs`), and whatever would take it past `AHEAD_NS`. Render targets are read from
their descriptors (format, view format, sample count), which is what the replay resolves them to unless a target is left
out; a guess that misses costs two libraries at most.

- `AH`:
  - `int on, live`: on: -1 until read; live: `vk_exec_record` walks `handles[i]` now.
  - `const op_hdr *at[CALL_DEPTH + 1]`: the op being replayed at each call depth.
  - `uint64_t seq`: the submission scanned last.

### `vk_exec_ahead()`

- `if (!AH.on || !AH.live || evo_in || rw_chunk || AH.seq == vk_exec_submit_seq) return`: not in a chunk.
- `static VkExec sx`: a copy of the replay state: too large for the stack.
- `if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) break`: the replay reports it when it gets there.
- `if (depth > 0) h = ret[--depth]`: back in the caller: on from the op after its call, below.

### `op_linear()`

Ops that only store state or draw: nothing they record runs outside a pass, and the walk goes on with the op after them.
Counter reports and resets are among them: they close and open an occlusion query inside the pass, and the value reaches
guest memory from the completion thread once the submission's fence has passed
(`runtime/vk/vk_exec_xfer.c` `oq_cb()`, `counter_cb()`), never from the GPU inside the run; so are the render
conditions, which the replay decides on the CPU. Ocarina reports a counter after most of its indirect mesh draws. Any
other op the walk replays bumps `vk_exec_brk`, and so does a held submission that goes on in between (`evo_resume`):
what was prepared for the draws of a run of such ops (`runtime/vk/vk_exec_draw.c`, the indirect mesh draws' records)
holds only while it stays the same.

### `vk_exec_mesh_run_stops()`

- `mesh_run_stop`: the op each run of `vk_exec_mesh_run` ended at, since the start.

For the perf log: the ops the runs ended at most often.

### `vk_exec_brk_now()`

The indirect mesh draws that follow the op being replayed (cur: its payload) with only ops of `op_linear` between them,
`MESH_RUN_OPS` at most: their payloads into out, at most max. 0 when the walk is not at cur.

### `vk_exec_mesh_run()`

- `const op_hdr *h = rw_chunk ? rw_op : AH.at[AH.depth]`: a chunk reads on from its own op (into the next ones).

### `op_one()`

One op of the stream other than those that steer the walk (END, JUMP, CALL and `WAIT_EVENT`: walk); timed by its code
with `NX2SARIA_REPLAY_PHASES` (`vk_exec_phase_op`).

- `static int timed = -1`: read once: off, an op costs one more load and branch.

### Replay workers

(`NX2SARIA_REPLAY_WORKERS`, default auto: as many as the CPU's cores make worth it, `vk_exec_replay_workers_auto`; 0:
the replay records every op on the submitting thread; the Game tab's CPU helper threads row sets it) 3 workers: on an RX
7900 XT with a Ryzen 7 5800X3D, Hyrule Field standing went from 41.2 to 50.4 fps (2026-11-12, one batch), with the same
pictures there and in Kokiri Forest, Kakariko, Goron City, a graveyard house and Gerudo Valley; 5 or 7 workers measured
the same as 3. Players' games crashed with 3 on by default (0.9.1, 0.9.2): an RX 6700 XT lost its device after about
18,000 frames, its last mark the game's own compute dispatch (`op_dispatch`: 20x1x1 groups of cs d7566eb1, a depth
probe) and nothing after it marked, since the group of draws that followed was recorded by the workers, whose passes
have no marks; an occlusion query slot was found unavailable after its fence just before. An RX 6600 lost it the same
way twice (0.9.2). The cause is not known; auto gives that driver no workers (`vk_exec_replay_workers_auto`), and auto
is the default again since 0.11.0. A submission's draws come in runs between ops that have to stay in the replay's order
on its own thread (dispatches, copies, texture and buffer clears, events, the game's barriers, calls). Hyrule Field,
standing (`NX2SARIA_REPLAY_RUNS`, 2026-11-11): about 3,500 draws a frame in 82 runs, 96% of them in runs of 16 draws or
more and 67% in runs of 256 or more, with a timestamp report beside nearly every draw and a render condition beside
every second one. A run is cut into groups: a group ends where the render targets change after its first draw or clear,
and before an inline uniform update (and a mesh draw with the mesh fallback or while `NX2SARIA_GPU_PASSES` copies their
counts), which the submitting thread replays itself in the order of the stream like everything outside groups. A group
of 2 * `CHUNK_DRAWS` draws or more is recorded by several threads at once:

- The front pass, on the submitting thread, walks the group in the order of the stream before anything of it is
  recorded: it applies the state and binds, settles the CPU side of counter reports and decides the render conditions
  (`vk_exec_xfer_front`), and copies the replay state at the first op of each chunk;
- The chunks are taken in order by the workers and the submitting thread, each recording into a command buffer of its
  own pool with a draw context of its own, from its copy of the state, behind the full barrier that ended the replay's
  command buffer before the group, and ending behind a full barrier of its own. `vk_sync_append` puts them after the
  replay's command buffer in the same part, in the order of the stream, and the replay goes on in a new one.

The frame's pass hooks (`vk_contact_pass_begin`, `vk_dlaa_pass`) follow the frame pass by pass: they run in chunk 0,
which reaches the group's pass first, and the others start after that pass began; groups follow one another. What still
depends on the order between chunks waits for its turn (`vk_exec_turn`: every earlier chunk of the group done): a
texture lookup that would make or take back an entry (`runtime/vk/vk_texture.c` `entry_of()`) returns nothing and the
chunk replays the op again once in turn, and so do the contact shadows' pass and the HDR pass's source. Per-object
motion lists its draws under a lock (`runtime/vk/vk_exec_draw.c` `mvobj_draw()`), and the occlusion queries' slots and
segments follow the stream (`runtime/vk/vk_exec_xfer.c` `vk_exec_xfer_slots()`, `vk_exec_xfer_group_merge()`).
`NX2SARIA_REPLAY_PIPE=N` (1-4, default 0): the submitting thread hands the group to the workers, takes chunks of it as
they do until none is left to take, and goes on with the ops after it, in a new command buffer after the chunks' places
in the part (`vk_sync_append_later`), while the last chunks are recorded; also into the front pass of the next group: up
to N (at most `LIVE_GROUPS`) groups are handed out and not in their part yet, their chunks taken from one queue in the
order of the stream (Q). Hyrule Field, 3 workers (2026-11-12): the frame's groups took 6.8 ms of the submitting thread
and the ops between them 4.6. A group's chunk 0 runs its pass hooks after the group before began its pass, once (a pass
it begins again later would run them after those of passes further on, `runtime/vk/vk_pass_hooks.h`), and waits for
every earlier group where a hook acts; a turn (`vk_exec_turn`) waits for every earlier chunk, of earlier groups too. The
submitting thread waits for the groups (`group_wait`), and puts their command buffers and query slots in place
(`group_drain`, oldest first: `groups_put_done`), only where the order needs it:

- For room when N groups are out, before a part is split off or submitted (a part due before a group goes to the GPU
  first: its work would otherwise wait for the group's last chunk too), and before an op that chunks never record
  (`pipe_op`: copies, clears of textures and buffers);
- Where a pass's hook acts: DLAA's run (`runtime/vk/vk_dlaa.c`: the jitter and its end, and the per-object motion lists
  it takes) and the contact shadows' targets (`runtime/vk/vk_contact.c`) change what earlier draws read
  (`vk_exec_hook_turn`);
- When a lookup would make a texture entry or take back a retired one: the op returns early and goes again once the
  groups are done (`op_again`), as a chunk's does in turn (`runtime/vk/vk_texture.c` `lookup_quiet()`: what changes
  nothing, or takes back a released render target that kept its image, needs no turn);
- Uniform snapshot drops count from where they are in the stream (`vk_exec_epoch`: a group's chunks do not see the drops
  of ops after the group), the texture pool set syncs under the lock the chunks take, a chunk reads only the inline
  uniform updates before its group (`vk_exec_upd_top`), the HDR present's source goes by stream position
  (`vk_exec_stream_pos`), and the queries after a group's last report go into the next segment made after its front pass
  (`runtime/vk/vk_exec_xfer.c` GS).

Locks a chunk takes, the submitting thread takes too while it is behind (`vk_exec_shared`). The chunks it records, at
the hand-out or while it waits between two ops, use a draw context of its own (`RW.help_dc`). Off by default: Hyrule
Field, 3 workers, standing (2026-11-12), it shortens the replay (10.9-11.5 against 11.7-12.1 ms a frame) but not the
frame: with 1, 2 or 4 groups out the fps stayed within one of the 50 the stage 2 mode makes, over three batches. A frame
there is the replay (about 11.5 ms) and then the GPU's work after it (about 6.6 ms: the temporal upscaler about 4, the
write-back copies 1.5), which the next frame waits for (`runtime/nvn/nvn_api_core.c` `nvnQueueSubmitCommands`,
`NX2SARIA_SUBMIT_FINISH`; without that wait the game waits as long itself, 52.4 fps); during the replay the GPU keeps up
with what is submitted. Sending the part's command buffers up to the first chunk place still being recorded while later
groups record (tried) did so for at most 0.2 parts per submission and gained nothing.

- `CHUNK_DRAWS`: draws a chunk takes at least.
- `GROUP_PEEK`: ops read ahead for a group, at most.

### `run_op()`

The ops a run may hold; JUMP is followed.

### `ser_kind()`

- `RwWorker`:
  - `int idx`: also its command pool (`vk_sync_worker_begin`); the submitting thread's is RW.n.
  - `void *dc`: its draw context (`vk_exec_draw_ctx_new`).
  - `volatile int32_t sleeping`: waits for wake rather than spinning.
- `RW`:
  - `int n`: workers; 0 off, -1 not read yet.
  - `int pipe`: `NX2SARIA_REPLAY_PIPE`: the groups out at once at most, 0 none.
  - `int small`: `NX2SARIA_REPLAY_SMALL`: the fewest draws of a group one worker takes whole.
  - `uint64_t small_groups, small_draws`: such groups and their draws since the last perf line.
  - `void *help_dc`: the submitting thread's draw context for chunks it takes while it waits.
  - `const op_hdr *skip_to[CALL_DEPTH + 1]`: per call depth: the walk read ahead to this op, a group too small.
  - `uint64_t groups, group_draws, chunks, serial_draws, wait_ns`: since the last perf line.
  - `uint64_t end_draws[5]`: draws of groups by what ended them (`group_peek`'s `GE_*`), since the last perf line.
  - `uint64_t small_end[5]`: ... of those too small to split.
  - `uint64_t run_end[OP_COUNT_]`: draws of groups that an op outside groups ended, by that op.
  - `uint64_t ser_n[4], ser_ns[4], front_ns, own_ns, spin_ns`: the submitting thread, since the last perf line: ops
    outside groups by kind (`SER_*`), and in groups the front pass, its own chunks and the wait for the others'.
  - `uint64_t pw_n[PW_N], pw_ns[PW_N], behind_ns, pd_op[OP_COUNT_]`: `NX2SARIA_REPLAY_PIPE`, since the last perf line:
    waits by why (`PW_*`), the time from a group's hand-out to the end of the wait for it, and the ops that waited
    (`PW_OP`).
  - `volatile uint64_t turns, turn_ns, redos, chunk_ns`: `chunk_ns`: the chunks' recording, every thread's.

### Side work

(`vk_exec_par`): the replay thread hands the workers a loop over n items while they have no chunk to record, at a
submission's start (`runtime/vk/vk_usnap.c` `copy_regions()`). One job at a time, in a static slot: seq is odd while it
runs. A worker counts itself in (PJ.in) and checks seq again before it reads the job, and the caller, once every slice
is done, makes seq even and waits until no worker is in, so none reads a job that has ended.

### `vk_exec_par()`

- `__atomic_add_fetch(&PJ.seq, 1, __ATOMIC_SEQ_CST)`: odd: running.
- `__atomic_add_fetch(&PJ.seq, 1, __ATOMIC_SEQ_CST)`: even: ended.

### `rw_main()`

- `plat_thread_priority(PLAT_PRIO_HIGHEST)`: as the recording thread (`rec_claim`).
- `perf_lock_timing_here()`: `NX2SARIA_LOCK_TIME`, as on the recording thread.
- `vk_exec_phase_worker()`: `NX2SARIA_REPLAY_PHASES` counts this thread's ops as a worker's.
- Spins about 100 us after a chunk (the next group of the frame is usually closer), then sleeps.

### `vk_exec_replay_workers_auto()`

`NX2SARIA_REPLAY_WORKERS` = auto: the workers this PC gets. The game's own threads, the submitting thread, the present
thread and the pipeline builders already keep about half of the cores busy, and on the 8-core CPU above 3 workers gave
all there was (5 and 7 measured the same), so the count is half the physical cores less one: 2 on 6 cores, 3 on 8, 5 on
12, at most 6. Four cores or fewer get none: a worker would take a core from the game's threads. AMD's Windows driver
for RX 6000 and older (`vk_caps.amd_sc`) gets none either, as that is where a player's device was lost with workers on
and the cause is not known; a number set in nx2navi.ini or the menu still applies there, with a warning in the log
(`rw_init`). Physical cores of the process's affinity (`plat_cpu_cores`), or half the usable logical processors where
that is unknown.

### `rw_init()`

- `} else if (RW.n && vk_caps.amd_sc)`: the player's number stays (`vk_exec_replay_workers_auto`).
- `int pipe = cfg_int("NX2SARIA_REPLAY_PIPE")`: the groups out at once.
- `if (RW.small >= 2 * CHUNK_DRAWS)`: a group that large is cut into chunks (`group_start`): no group would match.

### `groups_before_done()`

Every group before g has all its chunks done.

### `chunk_in_turn()`

Every chunk before c in the stream is done: those of earlier groups, and its group's earlier ones.

### `vk_exec_stream_pos()`

A chunk of a group comes after the ops before the group and after its group's earlier chunks; the submitting thread
after every chunk of the groups it handed out.

### `vk_exec_epoch()`

Uniform snapshot drops (`runtime/vk/vk_usnap.c`): a chunk's are its group's; the submitting thread's come after every
group it handed out. A chunk sees the drops of epochs up to its group's, the submitting thread every one.

### `vk_exec_upd_top()`

The inline uniform updates of the submission a draw may read (`runtime/vk/vk_exec_draw.c` `upd_read()`): a chunk only
those before its group.

### `vk_exec_hooks_wait()`

- `const Group *g = &GQ[Q.out % LIVE_GROUPS]`: the last handed out, whose first chunk ran after the others'.

### `vk_exec_hook_turn()`

- A group's hooks run in its first chunk, after its group's earlier chunks; the earlier groups' draws read what the hook
  changes.

### `vk_exec_turn()`

- `if (!c)`: the submitting thread: after every chunk of the groups it went on past.

### `views_release()`

The image views a command buffer made for its targets go when its submission is done.

### `group_peek()`

- `enum { GE_RUN, ... }`: What ended a group (`RW.end_draws`): the end of the run, a render target change, an inline
  uniform update, a mesh draw, the read-ahead limit.
- `vk_gpu_passes_capturing()`: `runtime/vk/vk_crumbs.c`: `NX2SARIA_GPU_PASSES` is copying this frame's counts.

The draws of the group that starts at h (it ends at `*end`, for the reason `*why`), reading at most `GROUP_PEEK` ops.

- Mesh draws stay on the submitting thread with the mesh fallback (its buffers are shared), with the mesh emulation (no
  `VK_EXT_mesh_shader`: its arena, queue and the part's pre-pass command buffer are the submitting thread's) and while
  `NX2SARIA_GPU_PASSES` copies their workgroup counts (one list).
- `if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) { *why = GE_RUN; break; }`: the walk reports it.
- `if (begun) { *why = GE_TARGET; break; }`: another pass: the next group.

### `groups_done_advance()`

The groups whose every chunk is done, in order (Q.done), once a group's last chunk is.

### `chunk_record()`

Records chunk c with the command pool `pool` (a worker's index; the submitting thread's is RW.n).

- `else if (!groups_before_done(g))`: the pass hooks follow the frame pass by pass: after the group before's.
- `const Group *p = &GQ[(g->seq - 1) % LIVE_GROUPS]`: not done, so not put in its part yet: still its slot.
- The submitting thread records with its own (the state's), or one of its own past the group (RW.pipe).
- The replay's command buffer before the group ended behind a full barrier, with every image in GENERAL.
- `if (vk_texture_deferred_take())`: a lookup would have made or taken back a texture entry: in turn, again.
- `if (!c->idx) __atomic_store_n(&g->hooks, 1, __ATOMIC_RELEASE)`: a chunk 0 that began no pass.
- `vk_exec_barrier(ex)`: the next command buffer starts after it, every image back in GENERAL.

### `group_wait()`

The submitting thread past groups it handed out (RW.pipe): until every chunk of them is done. With `help` (only between
two ops) it records the chunks no worker took yet itself. Then nothing of them runs beside it any more.

### `group_put()`

A done group's command buffers into their places in the part, its closed query slots into their segments.

- `if (ex->barrier_at == ex->recorded) ex->barrier_at += rec`: nothing since the chunks' closing barrier.

### `groups_put_done()`

The done groups not in their parts yet, oldest first; when every one handed out is, the submitting thread is no longer
behind.

### `group_room()`

Before the front pass of a new group: its slot holds a group put in its part (the oldest goes in first).

### `pipe_op()`

The ops the submitting thread replays past a group it handed out; before any other it waits for the group. The ops a
chunk records, the game's barriers, dispatches, and events (a wait reads memory, a signal's value goes there from the
completion thread): their lookups go again once the group is done (`op_again`), and they leave what the chunks read as
it was (`vk_exec_shared`, `vk_exec_epoch`, `vk_exec_upd_top`).

### `op_again()`

A lookup of op h would have made or taken back a texture entry before the group it went on past was done (the submitting
thread's `vk_texture_deferred`): once it is, h goes again, as a chunk's op does in turn.

### `group_start()`

The submitting thread, at op h of the walk: when a group large enough to split starts here, it is recorded by the
workers and this thread, and the walk goes on with the op that ended it, which is returned; otherwise h (the walk
replays the group itself, without reading ahead again before its end).

- `if (end == h) return h`: an op the submitting thread replays itself.
- `NX2SARIA_REPLAY_SMALL=N` (default 0, with `NX2SARIA_REPLAY_PIPE`): a group of N draws or more that is too small to
  cut goes whole to one worker, and the submitting thread goes on with what follows it without taking the chunk itself.
  Kokiri Forest standing, 5 workers, 4 groups out, the early tail on (2026-11-13): about 700 draws a frame were in such
  groups, 3.6 ms of the submitting thread; with 16 the replay took 10.2 instead of 11.0 ms a frame (63.2 against 62.2
  fps: the game then waits longer for the GPU, which is busy 83% of the frame there).
- A part due goes to the GPU before the group, with the groups still recording in it: its GPU work would otherwise wait
  for this group's chunks too.
- `vk_exec_barrier(ex)`: the command buffer so far ends behind a full barrier, every image back in GENERAL.
- The chunks' passes get no marks (`vk_exec_pass_begin`): a lost device's report names the group between the marks.
  After the barrier, whose end of an open pass ends that pass's mark and empties the note.
- The front pass.
- `if (vk_exec_xfer_counting() && !ex->render_disabled) c->nslots++`: at most one query each.
- `case OP_CLEAR_COLOR: case OP_CLEAR_DS: break`: recorded by the chunk.
- `default: op_one(ex, o); break`: state, binds, render targets.
- `vk_exec_xfer_group(-1)`: the segments stay listed for the workers until the group is put in its part.
- `for (int i = 0; i < nc; i++) g->c[i].slot0 = vk_exec_xfer_slots(g->c[i].nslots)`: in the order of the stream.
- Handed out: the workers (and without RW.pipe this thread) take the chunks in order.
- The replay's command buffer so far goes into the part, then the chunks' places, and the replay goes on in a new one at
  once.
- `ex->barrier_at = ex->recorded`: the chunks end with a full barrier.
- The submitting thread takes chunks too, as the workers wake up, and goes on once none is left to take: the GPU waits
  for the group's part until its last chunk is done.
- `if (small) RW.small_groups++, RW.small_draws += draws`: a worker takes it: this thread goes on at once.
- `vk_exec_draw_new_cb()`: the submitting thread's own chunks used its draw context.

### `walk()`

- `if (RW.skip_to[depth] == h) RW.skip_to[depth] = NULL`: the small group read ahead is behind.
- `AH.at[depth] = h, AH.depth = depth`: where `vk_exec_ahead` reads on from.
- `if (RW.n > 0)`: the perf line's split of the replay outside groups.
- Timed only for the perf line: two clock reads for each of the ~1,700 ops a submission replays here were 4 % of the
  submitting thread's replay in Hyrule Field.
- `if (vk_exec_signalled && depth == 0 && !evo_in) evo_resume(ex)`: their ops go through this walk's checks.

### `rec_claim()`

- `rec_spent_ns`: time this thread has spent in `vk_exec_record`.

The replay records on the thread the game submitted from, and the kernel schedules that like any other guest thread: a
preemption in the middle of a part leaves the submission late for the frame it belongs to (Kokiri Forest: 274 of 2,737
frames had a late submit). The recording thread therefore keeps the highest priority so the scheduler prefers it over
the guest's other cores (`NX2SARIA_RECORDER_PRIO=0` leaves the guest's priorities alone) and gets a core of its own
(`plat_cpu_reserve`): every other thread of the process, the guest's cores, the audio, the shader and pipeline workers,
is kept off it, so nothing of this process preempts the recording. That core is then lost to the rest, so
`NX2SARIA_RECORDER_CORE=auto` reserves it only with 12 or more logical processors, and not while the process runs on the
die with the largest L3 alone (`runtime/diag/perf.c` `cpu_placement()`): on a two-die X3D part the game's threads
already have that die to themselves, and a core taken from them cost frames; 1 always, 0 never. Linux reserves none with
fewer than four fast cores. 0 is the default: on an i9-14900KF (8 performance and 16 efficiency cores, 32 logical
processors) with the replay on its own thread, the reserved core (logical processors 14 and 15) left the woken replay
thread unscheduled for about 200 ms roughly once a second while the game stood in Kakariko: its queue held 40 to 50
items, its lock was free, the wakes had been sent and it used no CPU time, while the game waited on a fence of the work
in that queue (27 frames a minute with a 150 ms or longer frame). With no core reserved the same minute had none. The
recording thread is not the first to call `vk_exec_record`: the game's main thread submits first and goes on submitting
a little at a time, while another guest thread does the work (Kokiri Forest: that thread spent 21.9 of the 22.5 s the
replay took in a minute and a half, the first one 0.5 s). The first thread to have spent `REC_CLAIM_NS` in
`vk_exec_record` is the one that keeps both.

### `vk_exec_tail_begin()`

The tail begins here (the early tail): what was recorded so far waits in the chain, the tail gets a command buffer of
its own. Not in a chunk or a continuation, nor with groups still out: the cut needs every piece of the part.

- `if (Q.drained != Q.out) group_drain(ex, PW_OP)`: the cut needs every piece of the part.
- `vk_exec_barrier(ex)`: the command buffer so far ends behind a full barrier, every image back in GENERAL.

### `vk_exec_record()`

- `if (vk_device_is_lost()) return 0`: the run is ending (`runtime/vk/vk_device.c` `vk_device_lost_check()`): nothing is
  recorded.
- `const int ph = vk_exec_phases_on()`: `NX2SARIA_REPLAY_PHASES`: the call's parts (`vk_exec_phase_seg`).
- `perf_lock_timing_here()`: `NX2SARIA_LOCK_TIME`.
- `if (prof_replay_tid != plat_thread_id()) perf_replay_thread()`: the profiler follows the thread replaying now.
- `const tex_t *keep[9]`: the render targets carry over to this submission (`set_rt` compares them by pointer).
- `ex->rt_resolved = 0`: images may have changed since the last command buffer.
- `ex->nfbl = 0`: the previous command buffer ended with a full barrier: every image is in GENERAL.
- `__atomic_store_n(&vk_exec_submit_seq, vk_exec_submit_seq + 1, __ATOMIC_RELAXED)`: read without the lock too.
- `split_at = split_n < 0 ? 0 : split_n ? split_n : INT_MAX`: 0 until `maybe_split` reads the settings.
- `vk_exec_event_sync(1)`: before the mirror sync and the uniform copy: a CPU signal after this point came after them.
- `int synced = n || (evo_pending && held_sync() && vk_exec_held_ready())`: `held_sync`.
- `if (evo_pending)`: the uniform snapshot covers what held submissions bind too: they may go on here.
- `if (evo_pending) evo_resume(ex)`: signals the CPU gave since, and holds past `CONT_MAX`.
- `if (evo && evq[slot].head)`: the queue waits: this submission lines up behind it.
- `cont_push(slot, cont_new(handles, n, i, stop))`: the rest of this submission waits for its signal.
- `if (ex->recorded) vk_exec_barrier(ex), vk_exec_pool_marks(ex)`: later submissions see this one's writes.
- `NX2SARIA_WB_SPLIT`: the write-backs in a part of their own, after one that signals the executed timeline
  (`runtime/vk/vk_sync.c` `exec_tl`): the wait after a frame's first submission then ends when the submission's passes
  are through, not its write-backs. Off by default: `runtime/vk/vk_sync.c` says what else that wait then leaves out.
- `int tail = 0`: the early tail: the write-backs go before the tail, with the executed timeline's signal.
- `vk_mem_live_swap(live | TL.live0)`: the submission's own note, as it would have been.
- `if (synced) ex->recorded += vk_mem_mirror_end(ex->cb, fence), vk_usnap_end(fence)`: VRAM mirror bytes the GPU wrote
  go back to guest memory.
- `part_start = ex->recorded`: the last command buffer holds nothing: the tail is in the chain before it.
- `if (xw)`: `NX2SARIA_WB_SPLIT`: what the wait on the executed timeline left behind.
- `for (int k = 0; k < 6; k++)`: the ops outside groups that ended the most, by their groups' draws.
- `for (int k = 0; k < 5; k++)`: the ops that waited the most often.
- `return ex->recorded - part_start`: what the last part holds.

## `runtime/vk/vk_exec.h`

The op-stream replay of the Vulkan back end. `runtime/vk/vk_exec.c` walks the command handles of a submission, keeps
render targets, passes, clears and fixed-function state; draws, binds and dispatches (`runtime/vk/vk_exec_draw.c`) and
copies, counters, events and conditional rendering (`runtime/vk/vk_exec_xfer.c`) plug in through the two hooks at the
bottom. Runs on the submitting thread under `vk_sync_submit`'s lock: no locking inside.

- `enum { VK_EXEC_DIRTY_VIEWPORT, ... }`: Dynamic state the next draw must record (`VkExec.dirty`). `vk_exec_record`
  sets them all at the start of each command buffer; the state ops set theirs; `runtime/vk/vk_exec_draw.c` clears what
  it recorded.
  - `VK_EXEC_DIRTY_VIEWPORT = 1 << 0`: viewports or depth range.
  - `VK_EXEC_DIRTY_DEPTH_BIAS = 1 << 2`: polygon offset.
  - `VK_EXEC_DIRTY_STENCIL = 1 << 4`: compare and write masks, references.
- `VkExecTarget`: One render target of `OP_SET_RT`, resolved to its `runtime/vk/vk_texture.c` image for the current
  command buffer.
  - `const tex_t *tex`: texture pin of `ex->rt` (`nvn_pins_sweep` keeps it); NULL: no target in this slot.
  - `VkImage image`: `VK_NULL_HANDLE`: no target or not renderable (logged once).
  - `VkFormat format`: view format: the view's when it reinterprets a same-size format.
  - `VkImageViewType view_type`: 2D, or `2D_ARRAY` for more than one layer.
  - `uint32_t level, base_layer, layers`: subresource of the view; of a 3D image: depth planes.
  - `uint8_t is3d`: a 3D image (colour only), viewed through 2D views of its slices.
  - `uint8_t blocks`: a BC image viewed as one texel per block (`VkTexImage.block_views`).
  - `uint32_t width, height, samples`: extent of that level; with blocks, in blocks.
  - `int32_t nvn_format`: `NVNformat` of the view (`VkPipeState.color_fmt` / `depth_fmt`).
  - `const VkTexImage *img`: the image (`fb_usage`, `fb_learned`).
  - `uint8_t scale`: `VkTexImage.scale`.
- `vk_exec_cond_frame_end()`: `runtime/vk/vk_exec_xfer.c`: a present, in replay order (`NX2SARIA_LOG_RCOND`).
- `vk_exec_cond_why()`: `NX2SARIA_LOG_RCOND=ab`.
- `VkExec`: The replay state. One instance lives for the run: NVN state is queue state and carries over from one
  submission to the next. The "per command buffer" fields are reset by `vk_exec_record`.
  - Below, per command buffer:
  - `struct DrawCtx *dc`: `runtime/vk/vk_exec_draw.c`: the draw context this state records with (NULL until a draw op
    names it: the calling thread's, `vk_exec_draw_ctx_use`).
  - `VkCommandBuffer cb`: being recorded.
  - `uint64_t fence`: timeline value the submission signals (`vk_completion_add`).
  - `int recorded`: Vulkan commands recorded so far; every handler adds the ones it records.
  - `int in_pass`: inside `vkCmdBeginRendering`.
  - `uint32_t dirty`: `VK_EXEC_DIRTY_*`.
  - `VkPassMark *mark`: Pass marks (`runtime/vk/vk.h` `vk_pass_mark_begin()`): the open pass's record (NULL: none or
    marks off), `vk_exec_frame.draws` when it began, and the passes and dispatches this submission marked so far.
  - Below, render targets (`OP_SET_RT`) and the current pass:
  - `p_set_rt rt`: last `OP_SET_RT` payload (a change ends the open pass); rt.n 0, no depth: none.
  - `int rt_resolved`: `color[]` and depth hold this command buffer's images for rt.
  - `VkExecTarget color[8], depth`: resolved targets.
  - `uint32_t pass_w, pass_h, pass_layers`: render area and layer count of the open (or next) pass.
  - `uint32_t pass_scale`: its targets' `VkTexImage.scale`: game viewports and scissors are multiplied by it.
  - `int pass_noatt`: the pass has no attachment: its size follows the draws (`vk_exec_noatt_dims`).
  - `VkFormat pass_color[8], pass_depth, pass_stencil`: attachment formats of the pass
    (`VkPipelineRenderingCreateInfo`); `VK_FORMAT_UNDEFINED`: none.
  - `uint32_t pass_ncolor, pass_samples`: `colorAttachmentCount` and rasterization samples of the pass.
  - Below, fixed-function state (`runtime/vk/vk_exec.c` keeps it; `runtime/vk/vk_exec_draw.c` records it before draws):
  - `float vp[16][6]`: `OP_VIEWPORTS`: x, y, w, h, near, far.
  - `int has_vp`: a viewport was set; else the render area.
  - `int vp_jittered`: the viewport recorded last took the DLAA jitter (`runtime/vk/vk_dlaa.c` `vk_dlaa_viewport()`).
  - `int pass_jit`: a viewport of the pass open now took it: its targets' edges are filled at its end
    (`vk_dlaa_pass_end`).
  - `p_scissor scissor`: `OP_SCISSOR` (scissor 0).
  - `int has_scissor`: a scissor was set; else unbounded.
  - `float depth_range[2]`: `OP_DEPTH_RANGE`: overrides the viewports' near and far.
  - `uint32_t st_vmask[2], st_wmask[2], st_ref[2]`: `OP_STENCIL` per face (0 front, 1 back): compare mask, write mask,
    ref.
  - `float point_size`: `OP_BIND_STATE` kind 6: NVN point size for the support buffer; 0 = never set = 1.
  - `float line_width`: `OP_BIND_STATE` kind 7: NVN line width in pixels; 0 = never set = 1.
  - `p_poly_offset poly_offset`: `OP_POLY_OFFSET`: depth bias factor, units, clamp.
  - `p_prim_restart prim_restart`: `OP_PRIM_RESTART`: applied to indexed strip and fan draws (`op_draw_idx`).
  - `float blend_color[4]`: `OP_BLEND_COLOR`.
  - `int64_t copy_stride`: `OP_COPY_STRIDE`: row stride of buffer <-> texture copies (`runtime/vk/vk_exec_xfer.c`).
  - `const dpool_t *tex_pool, *samp_pool`: `OP_SET_TEX_POOL`, `OP_SET_SAMP_POOL`.
  - `VkPipeState pipe`: Pipeline inputs. `runtime/vk/vk_exec.c` writes `depth_clamp` (`OP_DEPTH_CLAMP`),
    `raster_discard` (`OP_RASTER_DISCARD`) and, at pass begin, `color_fmt`, `depth_fmt` and samples;
    `runtime/vk/vk_exec_draw.c` owns every other field (shaders, prim, `y_flip`, state objects, vertex input).
  - Below, `runtime/vk/vk_exec_draw.c` slots (draws, binds, dispatch): left empty by `runtime/vk/vk_exec.c`:
  - `const prog_t *prog`: `OP_BIND_PROGRAM`.
  - `NVNbufferRange vbuf[16]`: `OP_BIND_VBUF`.
  - `p_bind_buf ubo[8][18], ssbo[8][16]`: `OP_BIND_UBO` / `OP_BIND_SSBO` per NVN stage (6 task, 7 mesh) and index.
  - `uint8_t cs_push[1024]`: `OP_PUSH_CC`: NVN2 compute push constants (c0 from 0x10).
  - `uint64_t tex[8][128], samp[8][128], img[8][32]`: `OP_BIND_TEX` / `OP_BIND_SAMP` / `OP_BIND_IMG` handles per stage.
  - Below, `runtime/vk/vk_exec_xfer.c` slots (render enable, conditional rendering): left empty by
    `runtime/vk/vk_exec.c`:
  - `int render_disabled`: `OP_RENDER_ENABLE` false or a failed `OP_RENDER_COND`: clears and draws skip.
  - `p_render_cond render_cond`: last `OP_RENDER_COND`.
  - Below, internal to `runtime/vk/vk_exec.c`:
  - `void *views`: image views created for this command buffer (freed at its fence).
  - `int barrier_at`: recorded right after the last full barrier (-1: none yet).
  - `int bar_dirty, bar_passes, bar_dispatches`: `NX2SARIA_BARRIER_GAME` (`runtime/vk/vk_exec.c` `barrier_to_why()`):
    the last full barrier was one the replay itself needed (a copy, clear or fill of its own follows it), so the next
    pass or dispatch gets a full one too; passes and dispatches begun since the last full barrier.
  - `VkImage fbl_img[9]`: Feedback loops (`runtime/vk/vk.h` `VkCtx.feedback`,
    `runtime/vk/vk_exec_draw.c` `feedback_prepare()`). Learned targets are attached in
    `ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT`; they stay in it across consecutive passes and go back to GENERAL at the next
    full barrier that is not a pass begin.
  - `VkImage fbl_img[9]`: images in the feedback layout now.
  - `uint32_t fbl_mask`: attachments of the open pass in that layout: bit i colour i, bit 8 depth.
  - `uint32_t fb_dirty`: attachments of the open pass written by a draw or clear since the pass began or the last
    in-pass barrier.
  - `uint32_t fb_read`: attachments of the open pass a draw sampled (feedback layout) since the pass began or the last
    in-pass read-to-write barrier (`feedback_war`).
  - `const void *fb_fs`: `feedback_scan`'s memo: fragment shader, texture bind serial, pass serial.
  - `uint32_t tex_serial, pass_serial`: bumped by every texture or image bind and pass begin.
  - `uint32_t pre_pass`: the pass (`pass_serial`) the part's pre-pass command buffer serves (`vk_sync_pre`;
    `runtime/vk/vk_exec_draw.c` mesh emulation); a pass begins after it with another serial.
- `vk_exec_pass_begin()`: Begins dynamic rendering with the current targets unless a pass is open (a full memory barrier
  precedes it). Call before a draw or an attachment clear.
- `vk_exec_pass_end()`: Ends the open pass (no-op without one) and marks its targets GPU-written. Call before copies and
  dispatches.
- `vk_exec_barrier()`: Full memory barrier (all commands, all memory); ends the open pass first. Copies and dispatches
  order themselves with it. Skipped when nothing was recorded since the last one.
- `vk_exec_barrier_dispatch()`: The barrier before a game dispatch: a full one, or with `NX2SARIA_BARRIER_GAME` none
  unless the replay's own commands came since the last full barrier (`runtime/vk/vk_exec.c` `barrier_to_why()`). Ends
  the open pass.
- `vk_exec_barrier_log()`: `NX2SARIA_TRACE=perf`, every 600 frames: the barriers per frame by kind.
- `vk_exec_split_now()`: Ends the open pass and the part without handing it to the queue (`vk_sync_cut`): the next draw
  begins the pass again at the start of a new command buffer, which the part's pre-pass command buffer precedes. 0 when
  no more can wait.
- `vk_exec_inline_begin()`: `runtime/vk/vk_texture.c` (`NX2SARIA_TEX_ALIAS`): commands that must run at the replay's
  current position. `vk_exec_inline_begin` returns the command buffer being recorded after a full barrier (an open pass
  is ended; `*reopen` says so), or `VK_NULL_HANDLE` when this thread is not recording a replay. `vk_exec_inline_end`
  begins the pass again when it was open. `vk_exec_inline_buffer`: a device buffer for transfers, destroyed when the
  submission completes.
- `vk_exec_probe_inline()`: A test's hook (`runtime/gfx/tex_probe.c`): between a call with cb and one with
  `VK_NULL_HANDLE`, the calling thread counts as recording a replay into cb for submission `fence`, so
  `vk_exec_inline_begin` hands cb out. Called from the record callback of `vk_sync_submit_own`, where a replay would be
  recorded.
- `vk_exec_draw_ctx_new()`: `runtime/vk/vk_exec_draw.c`, the replay workers: a draw context of their own (what draws
  keep from one to the next while they record: `runtime/vk/vk_exec_draw.c` `DrawCtx`), and the one the calling thread
  records with from now on (NULL: the replay's).
- `vk_exec_draw_group_begin()`: `runtime/vk/vk_exec_draw.c`, before the chunks of a parallel group are recorded
  (submitting thread): the stand-ins are made and the texture pool set this submission took is brought up to date, which
  the chunks then only bind. Returns the inline uniform updates of the submission so far (`vk_exec_upd_top`).
- `vk_exec_upd_top()`: `runtime/vk/vk_exec.c`: of the submission's inline uniform updates (all), those a draw on the
  calling thread may read.
- `vk_exec_xfer_group()`: `runtime/vk/vk_exec_xfer.c`, parallel groups: `vk_exec_xfer_group` turns the front pass of the
  group in slot `group` on (it lists the group's occlusion segments) or off (-1); `vk_exec_xfer_front` replays a counter
  report or reset, render enable or render condition in the front pass (returns the condition's decision, 1 renders);
  `vk_exec_xfer_chunk_state` is the occlusion counting at a chunk's first op, which `vk_exec_xfer_worker` hands to the
  worker side with the chunk's decisions and its group's slot (on), or turns it off.
- `vk_exec_xfer_group_merge()`: After every chunk of the group in slot `group` is done (submitting thread, groups oldest
  first): the slots the chunks closed go into their segments, in the order of the chunks.
- `vk_exec_xfer_counting()`: The front pass: whether a draw here counts samples (a query slot), and n slots of the ring
  in the order of the stream for one chunk (the first is returned; `vk_exec_xfer_worker` hands them to the chunk's
  worker).
- `vk_exec_draw_new_cb()`: `runtime/vk/vk_exec_draw.c`: forget what the command buffer being recorded has bound
  (pipeline, sets, dynamic state, vertex buffers): the replay continues in a new one (`vk_sync_split`).
- `vk_exec_pool_split()`: `runtime/vk/vk_exec_draw.c`, before a part of submission fence goes to the queue
  (`runtime/vk/vk_exec.c` `maybe_split()`): 1 when it may go. The texture pool set it binds is then sealed and the next
  draw that needs the pools takes another; 0 when every other set is bound by this submission's earlier parts, and the
  replay keeps recording into the same part.
- `vk_exec_draw_shaders()`: `runtime/vk/vk_exec_draw.c`: the vertex and fragment shader of the last draw recorded into
  this command buffer (NULL: none yet).
- `vk_exec_pool_marks()`: `runtime/vk/vk_exec_draw.c` "pool marks": at the end of a submission that recorded work, after
  its closing barrier. Hands the mark copies the GPU completed to `runtime/vk/vk_texture.c` and, every few such
  submissions, copies the marks out and clears them.
- `vk_exec_bc_gone()`: `runtime/vk/vk_exec.c`: the stand-in a 3D BC render target is drawn through goes with its image
  (`vk_exec_image_gone`).
- `vk_hdr_track`: `runtime/vk/vk_hdr.c`: with `NX2SARIA_HDR=1`, notes a draw into a presented window texture
  (`vk_hdr_track`: set by the first present).
- `vk_exec_scissor()`: Scissor 0 clipped to the render area of the open pass; extent 0: nothing to draw.
- `vk_exec_feedback_war()`: `runtime/vk/vk_exec_draw.c`: orders the open pass's earlier sampled reads of the attachments
  in written (bit i colour i, bit 8 depth) before a draw or clear writes them; nothing when none of them was sampled
  since the last such barrier.
- `vk_exec_noatt_dims()`: pass size when no attachment is set.
- `vk_exec_targets_recent()`: 1: every target of the current pass (any 1: at least one, and not only the depth target
  while a colour target is rendered for the first time) is one the game renders frame after frame: rendered in the last
  few presented frames, or steady (`runtime/vk/vk_target_frames.h` `tfr_pass_recent()`); 0 otherwise, or when the pass
  has none.
- `vk_exec_steady_passes`: its answers of 1 that only a steady target gave (`runtime/vk/vk_target_frames.h`).
- `vk_exec_ahead()`: Looking ahead (`runtime/vk/vk_exec.c`): called by a draw that found its shader or pipeline missing,
  before it waits or is skipped. The rest of the submission being replayed is read once and the stage libraries of the
  pipelines its later draws need are queued to the workers (`vk_pipeline_hint`). `vk_exec_ahead_op`
  (`runtime/vk/vk_exec_draw.c`) applies one bind or draw op of that scan to sx, a copy of the replay state;
  `vk_exec_ahead_begin` starts a scan there.
- `vk_exec_ahead_draws`: draws the scans saw, and those whose shaders they looked up.
- `vk_exec_targets_describe()`: The current pass's targets with their formats, extents and the frames they were last
  rendered in (diagnostics).
- `VkExecFrame`: `NX2SARIA_TRACE=gfx`: replay counters of the current frame, logged and reset at each present
  (`vk_exec_frame_log`).
  - `uint64_t fb_draws, fb_barriers, fb_restarts, fb_layouts`: feedback loops: draws sampling a written target of their
    pass, in-pass barriers, pass restarts, layout transitions.
  - `uint64_t fb_war_barriers`: in-pass barriers before a write to a target an earlier draw sampled.
  - `uint64_t ub_updates, ub_pass_breaks`: inline uniform updates, and those that ended an open pass.
  - `uint64_t bind_calls, bind_ns`: `bind_resources`: calls and the time spent resolving and pushing their sets.
  - `uint64_t bind_tex_ns`: of `bind_ns`, the texture descriptors' lookups.
  - `uint64_t bind_buf_ns`: of `bind_ns`, the uniform and storage buffer descriptors' resolution.
  - `uint64_t bind_sig_ns`: of `bind_ns`, the per-draw binding signature.
  - `uint64_t bind_push_ns`: of `bind_ns`, the set packing, pushes and binds after the binding loop.
  - `uint64_t dyn_calls, dyn_left`: dynamic state calls recorded, and left out as unchanged (B.dyn).
- `vk_exec_bind_count()`: per-chunk or per-submission descriptor reuse count.
- `vk_exec_frame_written()`: `vk_texture_gpu_written`: t rendered or copied into.
- `vk_exec_recording_fence()`: fence of the submission this thread records, else 0.
- `vk_exec_pending_draws_take()`: The draws skipped since the last call because a shader or pipeline was still building
  (`runtime/vk/vk_exec_draw.c`), and whether a frame lacking them would be held back at its present
  (`runtime/vk/vk_backend.c` `frame_hold()`).
- `vk_exec_pool_loading()`: 1 while the texture pool makes the textures of an area load
  (`runtime/vk/vk_exec_draw.c` `pool_load_backlog()`): the frames drawn meanwhile show black stand-ins, and `frame_hold`
  leaves their presents out.
- `vk_dump_armed`: `NX2SARIA_DUMP=<frame>` (`runtime/vk/vk_dump.c`): between presents frame-1 and frame, every pass's
  targets and every dispatch's storage images are copied out and written as PNGs to `NX2SARIA_DUMP_DIR` (default
  "dump"), with passes.txt (targets, pipelines, textures, samplers) and images.txt (per image value statistics). The
  hooks run only when `vk_dump_armed` is set.
- `vk_dump_init()`: reads the environment; `vk_exec_record`'s first call.
- `vk_dump_pass_end()`: after `vkCmdEndRendering`.
- `vk_dump_draw()`: After a draw's binds; mesh: a mesh draw (`s->vs` is its mesh stage, `s->ts` its task stage or NULL).
- `vk_dump_mesh_begin()`: Around a mesh draw of the dumped frame (after `vk_dump_draw`): a primitives-generated query,
  listed in `mesh_prims.txt`. begin returns the query, or -1 when none was begun; end takes that value.
- `vk_dump_copy()`: before a copy or texture clear is replayed.
- `vk_dump_present()`: `vk_be_present`: waits for the dump frame's copies and closes the logs.
- `VK_C0_BYTES`: c0, the NVN driver constant bank of one stage (`runtime/vk/vk_exec_draw.c`; `NX2SARIA_FIX_C0=0` binds
  zeros instead): bank is `VK_C0_BYTES`, stage an NVN stage (0 vertex, 1 fragment, 5 compute), ssbo and ubo the 16 bound
  slots of that stage.
- `vk_exec_op_draw()`: Handler hooks. `runtime/vk/vk_exec.c` defines both WEAK: they stop with `rt_fatal` naming the op.
  `runtime/vk/vk_exec_draw.c`: `OP_DRAW`, `OP_DRAW_IDX`, `OP_DISPATCH`, `OP_BIND_*` (program, state, vattr, vstream,
  vbuf, ubo, ssbo, tex, samp, img), `OP_UPDATE_UB`, `OP_DRAW_IDX_IND`, `OP_DISPATCH_IND`. payload: `nvn_op_payload` of
  the record.
- `vk_exec_phase_worker()`: `NX2SARIA_REPLAY_PHASES` (`runtime/vk/vk_exec_draw.c`): a replay worker marks its thread
  (`rw_main`), and each frame's end in replay order reports every 600 frames (`vk_exec_frame_end`).
- `vk_exec_phase_op()`: `runtime/vk/vk_exec.c` `op_one()`: an op of the stream, by its code.
- `vk_exec_phase_record()`: `runtime/vk/vk_exec.c` `vk_exec_record()`: one call's time on its thread.
- `enum { SEG_PROLOGUE, ... }`: `runtime/vk/vk_exec.c` `vk_exec_record()`'s parts: before the walk (pins, events, the
  mirror's and uniform snapshot's begin), the walk (ops, front passes, groups), the groups' drain to the closing
  barrier, the mirror's and snapshot's end, the rest.
  - `SEG_PINS, SEG_EVSYNC, SEG_MIRROR_BEGIN, SEG_USNAP_BEGIN`: of the prologue: the pins' sweep, the event sync, the
    mirror's begin, the uniform snapshot's begin.
  - `SEG_PEEK, SEG_GBEGIN, SEG_FRONT, SEG_HANDOUT, SEG_OWN, SEG_N }`: of the walk, `group_start`: the peek ahead, the
    barrier and group begin, the front pass's loop, the hand-out (slots, the part, the queue, the wakes), the submitting
    thread's own chunks.
- `vk_exec_view_aspect()`: The aspect a sampled view of img reads (`runtime/vk/vk_exec_draw.c` `get_view()`): stencil
  for a view in STENCIL depth-stencil mode.
- `vk_exec_target_view()`:
  - `uint32_t level, uint32_t base, uint32_t layers, int blocks)`: kept for the image's life.
- `vk_exec_op_xfer()`: `runtime/vk/vk_exec_xfer.c`: `OP_COPY_B2B`, `OP_COPY_B2T`, `OP_COPY_T2B`, `OP_COPY_T2T`,
  `OP_CLEAR_TEX`, `OP_SIGNAL_EVENT`, `OP_REPORT_COUNTER`, `OP_RESET_COUNTER`, `OP_RENDER_ENABLE`, `OP_RENDER_COND`,
  `OP_CLEAR_BUF`.
- `vk_exec_wait_event()`: `OP_WAIT_EVENT` (`runtime/vk/vk_exec_xfer.c` "event order check"): counts the wait; 1 when its
  condition holds in replay order (the value in memory, or the signals replayed so far), 0 when its signal has not been
  replayed yet.
- `vk_exec_event_met()`: the same test without counting.
- `vk_exec_event_met_now()`: Any thread, without the replay's lock: the wait's condition on the value in memory now or
  the signals replayed so far, whatever submission another thread is recording (`vk_exec_held_ready`). A decision only.
- `vk_exec_signalled`: set by every replayed SignalEvent; `runtime/vk/vk_exec.c` clears it.
- `vk_exec_event_cpu()`: Events the CPU signals (`runtime/vk/vk_exec_xfer.c` "events the CPU signals").

  any thread: `nvnEventSignal` is about to write the event's value.
- `vk_exec_event_sync()`: `vk_exec_record`: its mirror sync and uniform copy are taken now / it ends.
- `vk_exec_event_cpu_known()`: the CPU has signalled this event before.
- `vk_exec_event_holds_now()`: an `OP_WAIT_EVENT`'s condition on the value in memory now.
- `vk_exec_gate_wait()`: `runtime/vk/vk_backend.c` `submit_sync()`, before a submission is recorded: waits for the CPU's
  signal when the submission begins with a wait for it (`runtime/vk/vk_exec.c` "the gate").
- `vk_exec_event_at_sync()`: `NX2SARIA_EVENT_AT_SYNC`.
- `vk_exec_event_late_take()`: waits only a CPU signal after the copies would have passed, since the last call.
- `vk_exec_event_stats()`: Since the last call, which resets them: waits, waits reached before their signal, those of
  them a later replayed signal met, and the sum of how many submissions later it came.
- `vk_exec_submit_seq`: replay thread: submissions replayed so far.
- `vk_exec_submit_seq_now()`: The same from a thread that does not hold the submission lock (`runtime/vk/vk_sync.c`
  `sub_lock`), under which it is written.
- `vk_exec_brk`: replay thread: bumped by every op that is not state or a draw (`runtime/vk/vk_exec.c` `op_linear()`).
- `vk_exec_brk_now()`: `vk_exec_brk`, or in a parallel group's chunk the chunk's own count.
- `vk_exec_evt()`: `NX2SARIA_EVO_TRACE=1` (diagnostics, `runtime/vk/vk_exec.c` "event order trace"): one token per step
  of the replay's event order.
- `vk_exec_evt_frame()`: the game's present call: the frame's tokens are closed and logged.
- `VkDescMem`: The memory report's descriptor pools (`runtime/vk/vk_memory.c` `outside_log()`): the replay's pools,
  spare ones included, the sets each holds, and the texture pool's sets (0 before it exists).
- `vk_exec_held()`: any thread: held submissions waiting for a signal (event order).
- `vk_exec_held_ready()`: ... and one of them can go on now (`runtime/vk/vk_backend.c` `present_held`); any thread.
- `vk_dlaa_swap_img`: `runtime/vk/vk_dlaa.c`: once DLSS has scaled the scene this frame, passes larger than the scene's
  light target `vk_dlaa_swap_img` sample DLSS's output `vk_dlaa_swap_view` in its place, so the game's scaling draw
  scales DLSS's picture.
- `enum { VK_AA_GAME, ... }`: `runtime/vk/vk_aa.c`, the anti-aliasing setting in place of the game's SMAA
  (`runtime/vk/vk_exec_draw.c` `aa_dispatch()`). `vk_aa_mode`: this frame's replacement, `VK_AA_GAME` when the game's
  SMAA runs as it is. `vk_aa_role`: which of the game's SMAA passes a compute program is, by the hash of its guest code.
  `vk_aa_skip_ok`: the edge and weight passes may be left out (the last composition was replaced). `vk_aa_record`: the
  replacement of the composition into cb, from src (the composite, sampled) into dst (the scene target's `R32_UINT`
  storage view) of w x h; 0 when it could not. `vk_aa_composed`: what became of the composition this frame.
- `vk_contact_pass_begin()`: `runtime/vk/vk_contact.c`: a pass about to begin (its targets resolved, no pass open), and
  a dispatch or copy about to be recorded.
- `vk_contact_pending()`: a contact shadows' pass waits for the next transfer outside a pass.
- `vk_exec_oq_draw()`: `runtime/vk/vk_exec_xfer.c`, samples-passed counting: a draw about to be recorded in the open
  pass (begins an occlusion query when counting and none is active), and the open pass about to end (ends the active
  query).

## `runtime/vk/vk_exec_draw.c`

Draws, binds and dispatches of the Vulkan op-stream replay: the strong `vk_exec_op_draw` behind `runtime/vk/vk_exec.c`'s
weak hook. The bind ops fill the `VkExec` slots; a draw opens the pass, resolves the bound program to translated shaders
(the translator's answers come from the bound state), asks `runtime/vk/vk_pipeline.c` for the pipeline, pushes (or
allocates) set 0 as `VkPipeInfo.bindings` says and records the dynamic state, vertex and index buffers and the draw. A
dispatch ends the pass and does the same for the compute shader. `UPDATE_UB` is a buffer copy from transient memory
between full barriers.

Per command buffer the replay keeps transient memory (support buffers, copied uniform ranges, generated indices),
descriptor pools, image views and the storage-buffer ranges the GPU may write; all of it is freed, and the ranges are
reported with `vk_gpu_written`, from one completion callback once the submission's fence is done (the write watch does
not see GPU writes). Nothing here locks: it runs on the submitting thread under `vk_sync_submit`'s lock.

Unbound vertex streams read a zeroed buffer. The support buffer layout is the translator's SupportBuffer.

- `NX2SARIA_TONEMAP_STANDIN`: `runtime/vk/vk_tonemap.c`, see `untranslated_dispatch_fallback`.
- `SUPPORT_BYTES`: the translator's SupportBuffer (std140): `alpha_test`, `is_bgra[8]`, `viewport_inverse`,
  `viewport_size`, ...; 1408 pcss and 1424 `shadow_spread` (translator patches 0010, 0011), 1440..1519 HDR
  (`support_fill`).
- `NVN_STAGE_TASK`: NVN2.

### `oneshot_wait_ms()`

`prepare_draw`: how long a one-shot draw waits for its shaders and pipeline before it is skipped. Such a draw renders
into a target that is neither recent nor steady (`runtime/vk/vk_target_frames.h`: a volume, a lookup table, an icon, a
target made new at an area change); skipped, a target the game draws once stays as it was. A draw into per-frame targets
whose shaders write memory takes the same wait (`writers_wait_on`). `NX2SARIA_ONESHOT_BUDGET` (default 1): the one-shot
draws of a presented frame wait from that frame's wait pool (`frame_wait`, shared with the full-screen draws), a mesh
draw `MESH_WAIT_MS` at most. Until 0.7.1 each waited up to `NX2SARIA_ONESHOT_WAIT_MS` (30 s) on the replay thread, which
is what `=0` still does: after a scene load every pass was new or not recent, so the first frame waited for one pipeline
after another, and for the whole compile of each mesh pipeline (0.2 to 1.3 s cold): 5-14 s on an RTX 4080 SUPER with no
pipeline recorded, and frames of 0.7 to 1 s at area changes of a played install (`runtime/vk/vk_target_frames.h` "Steady
targets"), while the pipeline workers sat idle. A draw skipped at the end of its wait comes back the next frame its
target is drawn, when the target is recent and the pipeline is built on a worker like any other; a target drawn only
once whose pipeline took longer than the pool stays as it was until the game draws it again. With the stage libraries in
the driver's cache a fast link takes 2 to 6 ms, so the pool covers the lookup tables of a load several times over.

### `oneshot_budget_on()`

What a mesh draw waits for is a few milliseconds when the caches hold it and 200 ms or more when they do not: the whole
build of a mesh pipeline without stage libraries, which has no fast link, or with them (`vk_pipeline_mesh_libs`) the
library of its task and mesh stages, which is most of that build. A longer wait only finds out which, and one that has
been building for longer than this is a compile: the draws that meet it in the frames after wait for nothing
(`oneshot_ms`). Before that rule a first visit waited 8 to 13 ms for each of 17 mesh draws over nine frames, up to four
in one frame, and every one of them was skipped at the end. The wait for a fast link (`link_budget`) needs the rule only
without stage libraries (`mesh_whole`): with them a mesh draw waits there as a vertex draw does, `LINK_DRAW_MS` from its
pipeline's first miss and nothing after.

### Resources of one command buffer, freed by its completion callback

- `Range`: back: written into a VRAM mirror (`vk_mem_mirrored`).
- `SPARE_POOLS`: Descriptor pools and full-size transient chunks go back to these lists when their command buffer
  completes, instead of being destroyed and created again for the next one: each creation is a driver memory allocation
  (on Windows an allocation, a map and a paging-fence wait on the CPU), about ten of them per frame inside the replay.
- `npools_live`: the replay's descriptor pools in existence, spare ones included (`vk_exec_desc_mem`).
- `REPLAY_POOL_SETS`: sets in each of them (`alloc_set`).
- `CbState`: State of the command buffer being recorded that the Vulkan command buffer itself holds (DrawCtx.B).
  - `VkPipeline pipe[2]`: bound per bind point: 0 graphics, 1 compute.
  - `VkPipeState st`: the state `vk_pipeline_set_dynamic` last recorded.
  - `VkDynShadow dyn`: the dynamic state values the command buffer holds (`prepare_draw_ex`).
  - `VkBuffer vb[17]`: vertex buffers last bound.
  - `int vcls[17]`: their pool class and pool record, for the census.
  - `NVNbufferRange vin[16]`: the ranges they were resolved from, under `vk_mem_gen` vgen.
  - `VkBuffer ib`: index buffer last bound, with its offset and index type (`bind_index_buffer`).
  - `uint8_t support[SUPPORT_BYTES]`: last support buffer and where it lives.
  - `float sup_in[3]`: the `support_fill` arguments it was filled from.
  - `uint8_t c0[5][VK_C0_BYTES]`: last c0 bank per stage (vertex, fragment, compute, task, mesh) and where it lives.
  - `struct { p_bind_buf ssbo[16], ubo[16]; uint32_t dim[3], binst; uint8_t push[1024]; } c0_in[5]`: what `vk_c0_fill`
    read for `c0[k]`.
  - `VkPipelineLayout ds_layout`: graphics layout the sets below were last pushed or bound with.
  - `VkDescriptorSet ds_bound[2]`: allocated set bound per set number (0: none or pushed since).
  - `VkDescriptorSet ds_last[2]`: last allocated and written set per set number, contents in DS.
  - `int ds_pushed[2]`: set number holds the push of DS.psig under `ds_layout`.
  - `int res_valid`: the graphics sets bound hold what the inputs in RS give (`bind_resources`).
- `DS_SIG_BYTES`: Contents of `B.ds_last` (`DrawCtx.DS`): the set's packed descriptors (`VkDescSlot`), which with its
  layout determine every write. A draw whose allocated set would hold the same reuses the set instead of allocating and
  writing one (a per-draw cost). psig: the same for the last push per set number; a push equal to it under the same
  layout is not recorded again (the descriptors stay bound across pipeline binds of that layout).
- `BS`: `NX2SARIA_BIND_STATS` counters (`bind_stats`).
- `RS_BYTES`: The inputs of the last graphics `bind_resources` (`bind_sig`, `DrawCtx.RS`) and what its build counted for
  the traffic census, which a skipped build counts again. `RS_BYTES` bounds the bound-slot bytes a pipeline reads
  (`RsList`): per binding at most one c0 storage table, otherwise one bound range or handle per descriptor.
- `RsState`:
  - `uint32_t hn`: words of h the last signature filled; 0: none.
  - `uint64_t h[24]`: generations, feedback images, support inputs, base instance.
  - `const VkPipeInfo *info`: the pipeline whose `RsList` ranges body holds, as of bind op generation gen.
  - `int rec`: a graphics build is running: `count_use` records, scratch copies mark volatile.
  - `int volatile_`: the build copied guest bytes into transient memory: a rebuild differs.
  - `int cmds`: check mode: pushes, binds and set writes the build recorded.
  - `uint32_t ckn`: check mode: the last build's packed descriptors (bytes in ck).
- `rs_on`: `NX2SARIA_BIND_SKIP`: 1 skip (default), 2 check, 0 off.
- `RS_PTRS`: The bound-slot bytes of `VkExec` a pipeline's descriptors read, as ranges merged where they touch, built
  once per pipeline info: c0's storage table, the uniform and storage ranges of the bound slots and the texture and
  image handles. sup and c0: the support inputs and the base instance count too. p: storage buffers whose address the
  shader loads from a user uniform block; the address read from the block counts too. ok 0: never skipped (too many
  descriptors or such buffers).
- `rs_lists`: `VkPipeInfo` pointer -> `RsList`, until shutdown.
- `STAGEM_N`: Types of the context's caches and memos; each is described where it is used.

  `find_stage`.
- `SLOTM_N`: `slot_mask_of`.
- `PB_N`: per-binding descriptor reuse (`pb_take`).
- `PbEntry`:
  - `uint8_t ok[PB_DESC]`: descriptor j holds a result that may be reused.
  - `uint64_t in[PB_DESC][4]`: the values it was resolved from (`pb_key`).
  - `PbCount cnt[PB_DESC]`: what its resolution counted for the census.

### The recording context

`bind_resources`: the sampled-texture descriptors this context resolved, each with the generations and slot token it was
resolved under: while its slot, the submission and the samplers are as they were (`slot_still`, as the texture
descriptor memo TD checks, without the lock) it holds what TD would give, and a hit takes no texture lock. The replay
workers' chunks waited for that lock at nearly every draw: 6.1 s of waits a minute in `bind_resources` alone (Hyrule
Field, 3 workers, `NX2SARIA_LOCK_TIME`, 2026-11-12).

- `DrawCtx`: What the draws, binds and dispatches keep from one call to the next while they record a command buffer: the
  state the command buffer holds (B), the signatures that let a draw skip its descriptor build or push (DS, RS, PB), the
  previous draw's shaders and pipeline (SM, PM), the shader lookup's caches (AC, stagem, slotm) and the submission's
  transient resources (res). A thread reaches the one it records with through dc; every thread starts with `dc_main`,
  the replay's, which is the only one while the replay records on a single thread. Shared tables (image views, shaders,
  descriptor lists, the texture pool sets, the stand-ins) stay file statics.
  - `CbRes *res`: transient resources of the submission being recorded (`cb_res`).
  - `uint64_t res_fence`: the submission they belong to.
  - `const VkPipeInfo *rs_last`: `rs_list`'s last lookup.
  - `uint32_t c0_dim[3], c0_binst`: the dispatch size and base instance of the draw or dispatch being bound.
  - `uint64_t bind_gen`: bumped by every bind op that writes `ex->prog`, ubo, ssbo, tex or img.
  - `AcSlots AC[8]`: `answers_for`: texture and image types per stage.
  - `StageMemo stagem[STAGEM_N]`: `find_stage`: the shader serving each NVN stage, per program serial.
  - `SlotMask slotm[SLOTM_N]`: `slot_mask_of`: the slots a (program, stage) translation reads.
  - `int sf_mvprev`: `shader_for` asks for the vertex shader's previous-frame variant (motion).
  - `int mfb_stage`: the mesh fallback binds this NVN stage's resources (`nvn_stage_of`).
  - `const GfxShader *mfb_cur_vs`: the mesh fallback's vertex stage (`prepare_draw_ex`).
  - `uint64_t mfb_list`: the address of its draw's indirect record, 0: none or not held (`mfb_hold_role`).
  - `PbCount *pb_cap`: per-binding reuse: where the resolving descriptor's census count goes.
  - `int pb_spoiled`: ... and it may not be kept.
  - `PbEntry *pb_e`: the build running, NULL when it keeps nothing.
  - `uint64_t pb_key_cur[4]`: `pb_take` to `pb_keep`: the descriptor's values.
  - `int pb_key_ok`: ... and whether it may be kept at all.
  - `int mo_ovr_on`: per-object motion: `mo_ovr` replaces uniform bindings (`ubo_info`).
  - `VkDescriptorBufferInfo mo_ovr[2][32]`: [vertex, `fragment][slot`]; buffer 0: not replaced.
  - `struct { int valid, has_fs; MemoKey k; const GfxShader *vs, *fs; } SM`: the shader memo (`shader_memo_on`).
  - `struct { int valid; VkPipeState s; const VkPipeInfo *info; } PM`: the pipeline memo (`pipe_memo_on`).
  - `void *fm`: `feedback_scan`'s memo per texture handle (`FmEnt[FM_N]`), made at first use.
  - `void *mo_vm`: `mvobj_pick`'s memo of the motion variant per vertex shader, made at first use.
  - `struct { const void *d; uint64_t ind; VkDeviceSize at; } ma[64]`: `mesh_ring_entry`: entries filled ahead.
  - `int ma_n, ma_i`: ... how many, and the next one a draw takes.
  - `uint32_t ma_brk`: ... `vk_exec_brk_now` when they were filled.
  - `TexMemo TM[TM_N]`: `bind_resources`: sampled-texture descriptors (`tm_take`).
  - `VkWriteDescriptorSet bind_w[MAX_BINDINGS]`: `bind_resources`: write structs, packed descriptors, texel views.
- `shader_lock`: Tables the chunks of a parallel group (`runtime/vk/vk_exec.c` "replay workers") share, locked only
  while a chunk records: outside groups the submitting thread alone replays, and these lookups run per draw.

  `shader_tab`.
- `rs_lock`: `rs_lists`.
- `pool_mt`: `pool_sync` from a chunk.

### `cb_done()`

Completion thread.

### `dispatch_mark()`

Marks a dispatch about to be recorded (`runtime/vk/vk.h` `vk_pass_mark_begin()`); the caller ends it after the dispatch.

### `scratch()`

- `HOST_COHERENT`: the CPU fills these chunks and never flushes them. Intel's integrated GPUs (ANV, Meteor Lake) offer
  host-cached memory only without coherence, which VMA picks for random host access: the GPU then read what an earlier
  submission left in the chunk (an UpdateUniformBuffer payload of another draw: a lighting shader's step count of
  0x3f800000 instead of 128, and a loop of a billion steps).
- `if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY)`: once more after a give-back.

### Stand-ins for what a binding names but the game did not bind

(zeros, a 1x1 image, the default sampler).

- `Z`:
  - `VkBuffer sink`: `ssbo_info`: an unresolved storage buffer a shader writes (zbuf stays zero).

### `make_dummy()`

A 1x1 stand-in image of view type `dinfo[i]` in `fmt`, moved to GENERAL and cleared to `clear` by the upload batch
(which reaches the queue before the submission that uses it). `*sview` is the view a storage descriptor takes (a cube
becomes a 2D array). Left uncleared its content was undefined, and a material that read its roughness map through one (a
slot with no texture yet) could see roughness near 0: on Kakariko's distant cliffs at night the specular term reached
3392 in single pixels, which bloom and the temporal upscaler spread into bright white spots.

### `storage_dummy()`

- `DUMMY_SAMPLED`: Sampled stand-ins read as mid grey, opaque: a missing roughness or metalness map gives a middling
  value, a missing two-channel normal map a flat normal, a missing colour map grey. Storage stand-ins (any format,
  written by shaders that have no image) start at zero.
- `tex_format`: Storage image stand-ins in the format a shader declares (`GfxShaderRes.format`: the translator's
  TextureFormat order); Vulkan wants the view format to match the declaration. Created on first use.

### `dummies_init()`

- `.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT}`: the memset below is
  not flushed.
- The samplers every other one falls back to (`tex_info_full`, `pool_sync_entries`): without them there is none.

### `vk_exec_draw_shutdown()`

- `mesh_ring_release()`: Releases the stand-ins and the depth twins. The device must be idle; the game path leaves them
  to process exit like the pipelines.

- `if (vk_ctx()) mesh_ring_release(vk_ctx()), pool_marks_release(vk_ctx())`: made without the stand-ins below too.
- `dc_main.rs_last = NULL, dc_main.rs_last_l = NULL`: the replay's context: no other records now.

### Resource types

### `gfx_type_of()`

`GFX_TEX_*` of a bound texture slot (the translator's `tex_type` / `img_type` answers).

### `vk_exec_view_aspect()`

A view in STENCIL mode samples the stencil plane (TextureViewSetDepthStencilMode); otherwise depth wins. The translator
declares every sampled image float and the shader takes the raw bits, as for integer formats.

### `int_view_format()`

- `ViewNode`: Image views of registered textures, kept for their image's lifetime: made per command buffer they were
  created again every frame (`vkCreateImageView` took about 8% of the fight replay on the 7900X). Used under the texture
  lock, as `vk_exec_image_gone` is (`destroy_tex`), which hands the views of a destroyed image to the deferred release
  with it.
- `view_of`: `ViewKey` -> `VkImageView`.
- `views_by_image`: `VkImage` -> its first `ViewNode`.

Integer formats a texture view may give an image (`nvnTextureViewSetFormat`): the Vulkan format and the translator's
TextureFormat code (`runtime/gfx/gfx_shader.c` `fmt_names`) the translator declares a storage image with. A shader that
packs texels itself writes raw words through such a view (Ocarina's last post-process pass stores R11G11B10 bits through
an R32UI view of an R11G11B10F target); through a view of the texture's own format the store converted them (a black
picture with red blobs). 0: not an integer format.

### `texel_bytes()`

Bytes per texel of the formats a storage view may reinterpret (`int_view_format`'s and the 32/64/128-bit float and unorm
ones they alias); 0 unknown.

### `raw_note()`

Images written this frame through an integer storage view (`storage_view_format`), in the order of their writes (seq):
Ocarina's last post-process pass stores shared-exponent RGB9E5 texels that way into its R11G11B10F scene target, raw
words the upscaler that reads them (tensor-core, untranslatable) decodes itself, and the fallback blit must too
(`raw_9e5`). Now and then (1 of 529 frames sampled in a field) the same pass writes them into the RGB9E5 image at that
address instead, as native texels (`raw_native_9e5`); the R11G11B10F image there then holds an older picture.

### `raw_written()`

The order of img's last write through an integer view, 0 when it had none this frame or the one before.

### `raw_9e5_twin()`

The image that holds this frame's post-process picture at t's place in memory, NULL when none was written: the
R11G11B10F image written as raw RGB9E5 words, or t itself when t is the RGB9E5 image and was written as native texels,
whichever was written last. The picture resolves as an RGB9E5 texture there, and that image is otherwise one nothing
renders into, stale or never initialised. Looked up live (texture lock held), as an image noted in RAW may be gone.

### `vk_exec_raw_9e5()`

`runtime/vk/vk_dump.c`: 1 when img was written as raw RGB9E5 words this frame (dump it decoded as such).

### `storage_view_format()`

The format a storage view of img takes: the view's integer format when it sets one of the image's texel size (the image
is `MUTABLE_FORMAT`, `runtime/vk/vk_texture.c`), else the image's own.

### `get_view()`

An image view of a registered texture for a shader resource of type `rtype` (`GfxShaderRes.type`); `VK_NULL_HANDLE` when
the image cannot serve it (the caller binds a stand-in). Texture lock held.

- `if (!ok && !storage && !ms && img->type == VK_IMAGE_TYPE_2D)`: a flat texture under a 3D sample: its 3D copy.
- Texture detail (`runtime/vk/vk_texture.c`): the image was made without the game's top `img->skip` levels, so the
  view's levels [l0, end) of the game are the image's [l0 - skip, end - skip). A view that names only levels the image
  lacks shows the first one it has: the nearest content, where a stand-in would show none.
- `if (!storage)`: view selectors replace the texture's, then the format remap.
- `if (img->block_views) ci.pNext = &use`: the image's `COLOR_ATTACHMENT` usage is its block views'
  (`runtime/vk/vk_texture.c`).

### `vk_exec_target_view()`

An attachment view of a registered texture, kept with its sampled views for the image's lifetime: made per command
buffer, every render target's views were created again each frame (`vkCreateImageView` under `vk_exec_pass_begin`, 2% of
the fight replay on the 7900X). Attachment keys carry a swizzle word no sampled key has (those hold four NVN selectors,
each below 8). The texture lock guards the tables, as for `get_view`; `vk_exec_pass_begin` may already hold it.

- `vk_texture_lock()`: `destroy_tex` sets gone and purges the image's views with it held.
- A texture destroyed while a command buffer or DLSS still held it (`img->gone`): its image lives on until the GPU is
  past the submission being recorded (`vk_defer_image`), so a view for this use is made, and goes the same way. Cached,
  it outlived the image: Vulkan hands the handle to a later image, which then got the view of the dead image's memory
  and the GPU faulted reading it at the start of a pass ("read invalid", a 128x128 scene capture, `#177`).
- `if (blocks) ci.pNext = &use`: not the BC image's sampling and transfer usages.

### `vk_exec_image_gone()`

- `vk_tonemap_forget(image)`: the stand-in's views of a game image it read as it is.

### Program to shaders

- `SHADER_SETS`: (program, stage, answers) -> shader, 4-way set-associative, most recently used first. A miss costs a
  SHA-256 of the whole shader code (`runtime/gfx/gfx_shader.c` `req_init()`), slow on CPUs without SHA instructions: a
  field frame looks up ~3000 and the 2048-slot direct-mapped table this replaced missed 3% of them on conflicts.

### `ctl_nvn2()`

Stage and code length of a control section. NVN1 (0x880 bytes): stage at +0x714, length at +0x704, c1 offset at +0x700,
workgroup size at +0x748. NVN2 (Switch 2, 0xe0 bytes, magic 0x98761234 at +0): stage in bits 16+ of +0x54, length at
+0x38, workgroup size at +0x58, no c1 part (`runtime/gfx/gfx_shader.c`, "NVN2").

### `ctl_stage_len()`

- NVN2: the c1 constants (BRX jump tables) follow the code from +0x40 to +0x44, in the same storage; the length covers
  them so `runtime/gfx/gfx_shader.c` `req_init()` sends them (with the code length alone it never could).

### `ctl_c1()`

Offset of the c1 constants (len: none).

- `else memcpy(&c1, c + 0x40, 4)`: NVN2: after the code and its padding, up to +0x44.

### `find_stage()`

Which shader of a program serves each NVN stage, and its code length, per program serial (new at every
`nvnProgramSetShaders`): the scan reads two words of every stage's control block, cold guest cache lines, and runs
several times a draw. Direct-mapped (`DrawCtx.stagem`); a collision only scans again.

### `token_of()`

Texture and image types of a stage's first 32 slots, each kept with the handle and the generation and slot token it was
looked up under: a slot is looked up again, a hash probe, only when the handle changed or its slot may resolve otherwise
(`slot_still`). `DrawCtx.AC`, per stage.

`vk_texture_slot_token` of the slot a texture or image handle names; an empty handle always resolves to nothing.

### `slot_still()`

Whether what handle h resolved to under the global generation `*egen` and its slot token etok (both read before that
lookup) still holds at global generation gen: nothing at all changed since, or not its slot (the token, read only when
the global generation moved, about every 16 draws). A token hit moves `*egen` to gen.

### `slotm_idx()`

The slots of (program, stage) whose types its translation reads (`gfx_shader_slot_mask`), learned from its first shader;
~0u while unknown. The types of other slots select the same shader, so `answers_for` leaves them zero instead of looking
up all 64 handles. Direct-mapped by serial and stage (`DrawCtx.slotm`); a collision only relearns.

### `type_of_handle()`

Texture lock held.

### `format_of_handle()`

The `img_format` answer of an image handle: the integer format its view gives it (`int_view_format`, as `get_view` binds
it), else 0 (the translator's default, a float format). Texture lock held.

### `answers_for()`

The answers of stage st for the slots in mask; the types of other slots stay zero.

- The generation and tokens are read before the lookups (one that creates an entry changes them) and unlocked: a
  registration racing with them is ordered after this draw, and a slot stored under older ones is only looked up again.
- `if (check && (type_of_handle(h) != c->tt[slot] || type_of_handle(hi) != c->it[slot]))`: `NX2SARIA_SHADER_MEMO=check`.

### `vk_exec_probe_shader()`

`*present`: the program has this stage. Returns NULL when it has not or the translation failed. pending (graphics
stages, `vk_pipeline_async_on`): a shader not translated yet is queued and NULL returned with `*pending` = 1.
`await_ms`: a shader queued earlier is waited for instead, that long at most (`gfx_shader_await`; `UINT32_MAX`: until it
is done), and `*pending` = 1 says the wait ended first. `DrawCtx.sf_mvprev`: `shader_for` asks for the vertex shader's
last-frame variant (per-object motion).

- `probe_sh`: Probes only (`runtime/gfx/mesh_fb_probe.c`): the shader of the program stage whose control section is ctl,
  in place of a translation. The probes build their programs from modules made here, which no translator has an input
  for.

### `shader_for()`

- `uint64_t h = (serial * 0x9E3779B97F4A7C15ull ^ (uint64_t)st) ^ nvn_hm_hash(&a, sizeof a)`: a is zero-padded.
- `int lk = vk_exec_shared()`: only while a parallel group records do several threads look up at once.
- `if (w)`: to the front.
- `else if (set)`: in front, the least recently used way drops out.

### Descriptors

The mesh fallback (below, "mesh fallback") runs a program's task and mesh stages as compute modules: while it binds
one's resources, a compute module's bindings are those of this NVN stage (6 task, 7 mesh), not of the compute stage
(`DrawCtx.mfb_stage`).

Per-binding descriptor reuse (`bind_resources`, PB): while a descriptor that may be kept resolves, `pb_cap` is where its
census count goes, so a later reuse counts the same bytes again; `pb_spoiled` is set when the resolution copied guest
bytes into transient memory or counted more than once, and such a descriptor is not kept (both in `DrawCtx`).

### `count_use()`

`vk_mem_count`, also kept per (class, use) while a graphics build runs (RS).

### `bind_range()`

A buffer descriptor for [addr, addr+size) of guest memory: its backing (a pool's VRAM mirror when it has one) when
aligned, a copy in transient memory when the backing is host memory with an unaligned offset. 0 when neither works.
written: the shader stores into it (a mirror's range goes back to guest memory at the end of the submission).

### `c0_ssbo_base()`

The driver constant bank c0 as GLSLC-compiled shaders read it. Storage-buffer tables {u64 address, u32 size} every 16
bytes at 0x110 (vertex), 0x510 (fragment), 0x310 (compute): the translator's GlobalToStorage fallback loads each
candidate base from here and compares the global pointer against it. Compute: dispatch size at 0x200, the uniform buffer
table at 0x210 + 16 r. Graphics: the largest point size (Maxwell's limit, GLSLC clamps `gl_PointSize` to it) at 0x610,
the base instance at 0x644. NVN2 task and mesh stages: storage tables at 0x670 and 0x770 (the game binds slots 0-2
there, and their programs read 0x670.. and 0x770..). The mesh table starts 0x90 bytes before the bank's end: it holds
the 9 slots that fit, and sixteen would write 0x6C bytes past the bank. Other stages: zeros. Unbound slots read zero,
since the replay keeps them zero. Byte offset of a stage's storage buffer table in c0, -1 for none.

### `c0_on()`

`NX2SARIA_FIX_C0=0`: c0 reads zeros instead of the driver constants.

### `c0_info()`

- `nvn2_cs_tables()`: c0 from replay scratch; a bank equal to the stage's previous one reuses its range, so it costs no
  copy.

- One bank per stage: the inputs below do not say which stage they are for, and the stages lay the storage table out at
  different offsets (`c0_ssbo_base`). Task and mesh shared the compute entry: the foliage's task and mesh stages bind
  the same storage buffers, so the task stage was handed the mesh stage's bank (its table at 0x770), read zero addresses
  at 0x670, culled every instance and launched no mesh workgroups.
- `vk_c0_fill` reads the storage slots, and the uniform slots and dispatch size for compute or the base instance for
  graphics: equal inputs give the same bank, so neither the fill nor the 2 KiB compare is needed. A vertex or fragment
  bank depends on the storage slots and the base instance alone (the rest of its inputs record is zero): those two are
  compared without building and comparing the whole 1.8 KiB record on every draw.
- NVN2 compute: the bank holds the push constants from byte 0x10 (then the program's handle table, which the host does
  not read: textures and images are bound by slot).
- The pushed constants from 0x10, the driver's tables after them (`push_ptr`, `nvn2_cs_tables_at`).
- Bytes 0-0xb of an NVN2 compute bank are the dispatch's group counts (x, y, z). The fog pass after the integration
  (passes 0115/0116 of a Kokiri frame) maps each thread to tid / (groups * 16 - 1): with zeros there every thread read
  the same point. An indirect dispatch leaves them 0 (`op_dispatch_indirect`).

### `limits_init()`

The buffer range limits, read once: the device's properties are a cold cache line on every binding of every draw.

### `ubo_info()`

Per-object motion (section at the end): while it records a draw again, these replace the uniform bindings of the vertex
stage's slots 18-20 (last frame's camera, bones and place) and the fragment stage's slot 3 (its parameters)
(`DrawCtx.mo_ovr`).

- `if (slot == 1 && ex->prog)`: compiler constants stored after the shader code.
- `if (ctl_nvn2(ctl)) memcpy(&len, ctl + 0x44, 4)`: NVN2: c1 is [+0x40, +0x44) after the code.
- `if (b->size && vk_usnap_find(b->addr, b->size < max ? b->size : max, align, &bi))`: as of the submit call.

### `ssbo_slot()`

The NVN slot of a storage binding. The translator numbers a shader's storage buffers in the order it meets them
(`GfxShaderRes.slot`); the NVN slot is the entry of the driver constant buffer c0 that holds the buffer's address
(`cbuf_slot` 0, handle = word offset): 16-byte entries at byte 0x110 (vertex), 0x510 (fragment) and 0x310 (compute). A
compute address at c0 0x210..0x30f is uniform block (offset - 0x210) / 16 read as storage: `*ubo` is set. -1: the
address is not in c0 (the shader loads it from a user uniform block: `ubo_ptr`) or the stage has no known table.

### `ubo_ptr_res()`

1 when the shader loads the storage buffer's address from a user uniform block (`cbuf_slot` 3.., NVN uniform slot
`cbuf_slot` - 3, byte offset handle * 4). Some terrain reads its vertices through one, about 500 draws a frame; with a
zero buffer they collapse and the ground is missing.

### `upd_note()`

- `UPD`:
  - `uint64_t fence`: the submission the entries belong to.
  - `uint32_t n`: updates noted in it.

The inline uniform updates of the submission being replayed, newest last (a ring of `UPD_N`). `op_update_ub` writes them
into the buffer's GPU copy; guest memory gets them only from the write-back when the submission ends. The terrain
rewrites its pointer block this way before every draw, all at one address, so `ubo_ptr` reads the latest update that
covers the pointer, else memory. Payloads stay in the recorded command buffer for the whole replay.

### `upd_read()`

1 when an update of this submission supplied [a, a+n).

- `uint32_t top = vk_exec_upd_top(UPD.n)`: a replay worker's chunk: the updates before its group.
- `if (a < e->lo || a + n > e->hi) return 0`: partly covered: memory.

### `ubo_ptr()`

That address as the bound block holds it at this point of the replay; 0 when the block is unbound or too short. The
block has no size field.

### `nvn2_cs_tables()`

NVN2 compute c0 (fitted to the storage offsets of 2549 compute programs against their control sections, round 24): from
byte 0x10 the constants the game pushed (PushComputeConstants), then a table of uniform buffers and one of storage
buffers, {u64 address, u32 size} every 16 bytes (counts: bytes 1 and 2 of control +0xb0), then 8-byte entries (byte 3 of
+0xb0, then byte 2 of +0xb4); +0xb8 (high half) is the end of it all, which gives the pushed size. Entry k of the
storage part is storage slot k (BindStorageBuffer), entry k of the uniform part uniform slot k. Progs 98 and 99 (the
compute after the composite: auto exposure) push 0x74 bytes, rounded to 0x80, and read storage entries 9 and 8 at
c0+0x120 / +0x1c0; most programs push nothing and have their tables at 0x10.

### `nvn2_cs_tables_at()`

Where the tables start: after the pushed constants.

- `if (!(b0 >> 8 & 0xffff)) return 0x10 + 1024`: no tables: all of c0 from 0x10 is the pushed constants.

### `push_ptr()`

An NVN2 compute program that takes a storage buffer's address from c0 (`c0_info`): an entry of the driver's tables (the
bound uniform or storage slot), or the {u64 address, u32 size} at that place of the constants the game pushed. 1 with
`*out` filled; 0 when the program is not NVN2 or the offset is in neither.

- `uint32_t o = (uint32_t)r->handle * 4`: c0 byte offset.
- `if (o < ta)`: in the pushed constants: a pointer the game passes itself.
- `*out = (p_bind_buf){0, 0, a, n < (1u << 30) ? n : 0}`: size 0: up to the end of the backing.
- `static int logged`: diagnosis: offsets past the tables.

### `ssbo_direct()`

The buffer a storage resource names by where its address lives (`GfxShaderRes.ptr` aside): an NVN slot of the stage, a
pointer in a user uniform block (size 0: up to the end of the backing), or an NVN2 compute push pointer. `*tmp` holds
the last two. NULL when it names none.

### `ptr_addr()`

The address of a storage resource read through a pointer table (`GfxShaderRes.ptr`): the u64 at offset ptr - 1 of the
buffer it names, as that buffer holds it at this point of the replay (like `ubo_ptr`); 0 when the buffer is unbound, too
short or not backed. Ocarina binds such a table as NVN storage slot 1 of most stages (five pointers at 0-0x20) and its
vertex, pixel and mesh shaders read their instance and light data through them (PORT-HANDOFF.md 7.17).

### `vk_exec_storage_addr()`

`runtime/vk/vk_dump.c`: the guest address a storage resource of NVN stage st resolves to, 0 when none.

### `ssbo_info()`

- The device address table (`runtime/vk/vk_gpa.c`, translator patch 0030). Mirrored pools are not synced for its
  readers: a sync of every mirror at a table reader's draw copied stale guest bytes over what the GPU had written into
  the mirrors earlier in the frame, and Kokiri Forest's lit areas went black. A pool read only through the table is
  rarely a mirrored one (mirrors are made for pools bound as buffers, which sync then). With shadow storage every pool
  the table names is a copy of guest memory, and nothing resolves the pools such a reader reaches: the binding is noted
  here, and the copies are brought up to date before the part is submitted
  (`runtime/vk/vk_memory.c` `vk_mem_shadow_table_read()`). A draw that reuses the descriptors of the one before it
  (`bind_resources`) comes after a build in the same command buffer, and the packed descriptors of `mo_desc` are built
  in the same guest submission as their draws: either way this ran for the guest submission.
- What the table names is mostly guest memory itself (pools without a mirror), which the GPU reads as it runs: the
  submission's submit wait is the GPU's (`vk_mem_take_live`). Without this the staged mirror uploads
  (`NX2SARIA_MIRROR_STAGE`) ended the wait once the replay had submitted, and the game wrote the next frame's instances
  while the GPU still drew from them: the small grass and moss, which reach their instances through the table, flickered
  and slid over the ground as the camera turned (RX 580, Kokiri Forest, 2026-11-11). Not every bind comes here (the bind
  skip, the kept descriptors): `gpa_reader` notes it at each bind.
- The mesh fallback's own buffers (`runtime/gfx/mesh_fb.c`): no guest memory behind them.
- `if (r->ptr) up.addr = ptr_addr(ex, st, r), b = &up`: size 0: up to the end of the backing.
- `runtime/vk/vk_caps.c`: shaders translated for `sbuf_align` address the buffer from its address rounded down to it.
- `static int logged`: diagnosis: what c1 of an NVN2 compute program is (its control section's c1 range, the.
- `const uint8_t *ctl, *code`: uniform slots the game bound).
- A shader that writes it gets the sink: the zero buffer also stands in for missing vertex buffers and read-only
  storage, and words written into it came back as garbage loop counts (a GPU hang at the first gameplay frame).

### Feedback loops ([maxwell.md](../maxwell.md) 1.9, `runtime/vk/vk.h` `VkCtx.feedback`)

The game's G-buffer and terrain shaders sample targets of the pass that renders them, at their own pixel, after earlier
draws of the pass wrote them (albedo, normals, R32F linear depth). Maxwell reads see those writes; in Vulkan the read is
undefined without a barrier, and on the RX 9070 it showed as dark screen-aligned blocks on the island ground and hid
Tulin's sage spirit. Before a draw whose fragment shader samples an attachment of the open pass that a draw or clear of
the pass wrote since the pass began or the last barrier:

- `VK_FEEDBACK_LAYOUT`: the image is learned (`VkTexImage.fb_learned`; it has the feedback usage since its creation),
  the pass restarts once with it in `ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT`, and from then on its passes attach it in
  that layout, its descriptors carry that layout, the draws that sample it use a pipeline with the feedback flag
  (`VkPipeState.feedback`) and a by-region barrier precedes such a draw.
- `VK_FEEDBACK_RESTART`: the pass ends and restarts behind the full barrier (`vk_exec_pass_begin`).

- `SubRange`: The image and the levels [l0, l1) and layers [a0, a1) a texture's view covers.

### `attached_as()`

Attachment slots of the open pass (bit 8 depth) whose subresource r covers.

### `knob_log_mesh()`

Knobs the draw and dispatch paths test on every call, read once: each cfg lookup walks the settings table
(`runtime/cfg/cfg.c` `raw_get()`), about 0.2 ms a frame of replay in Kokiri Forest when they ran per draw.

`NX2SARIA_LOG_MESH=1`: the first mesh draws' state.

### `knob_tm_dispatch()`

`NX2SARIA_TM_DISPATCH`: the scene upscaler stand-in's mode.

### `knob_log_cs_binds()`

`NX2SARIA_LOG_CS_BINDS` set: the compute bindings of each program's first dispatch.

### `td_mode()`

`NX2SARIA_TEX_MEMO` (`tex_info`, `feedback_scan`): 0 off, 1 on, 2 (`=check`) computes every hit again and stops on a
difference.

### `sampled_sub()`

- `FmEnt`: `DrawCtx.fm`: one table per recording thread.

The subresource a texture handle samples, per handle, kept while its slot resolves the same (`slot_still`): a draw's
texture bindings change the whole-scan memo below, but the handles they bind were mostly looked up before, in this pass
or an earlier one. Direct-mapped.

Texture lock held.

### `feedback_scan()`

Attachments of the open pass the fragment shader fs samples; kept while the shader, the texture bindings and the pass
stay the same.

- `uint32_t m = 0, gen = vk_texture_slots_gen()`: before any lookup.
- Only bound slots (0..127) are looked at. The texture and sampler pool arrays (res.pool: `VK_POOL_TEX` and 4096
  descriptors, bindless handles) have slot -1: walking them ran up to about 100,000 empty iterations per scan, about 3.6
  ms of replay per frame in Kokiri Forest, and read `tex[FS][0..127]` for them by mistake.
- `uint64_t tok = token_of(h)`: before the lookup.

### `feedback_prepare()`

Before the pipeline lookup of a draw with fragment shader fs: learns and restarts as the mode says. Returns the sampled
attachments in the feedback layout (the pipeline's feedback flag and `feedback_barrier`).

- `vk_exec_pass_begin(ex)`: learned targets now in the feedback layout; the full barrier cleared `fb_dirty`.

### `feedback_barrier()`

By-region barrier inside the pass: earlier attachment writes become visible to this draw's fragment shader reads.

### `vk_exec_feedback_war()`

Write after read inside the open pass: the GPU orders attachment writes after earlier attachment writes, not after
earlier draws' texture reads of the same image (feedback layout), so a draw or clear writing a target an earlier draw of
the pass sampled could land before those reads finish and they read the new value (a band of the picture a frame out:
the fog's flat colour, a dark shadow band). A by-region barrier from fragment shader reads to attachment writes orders
them; written: the attachments about to be written (bit i colour i, bit 8 depth).

### `feedback_writes()`

The attachments the draw with state s writes: colour targets with a nonzero effective write mask, depth with depth
writes or the stencil test on.

### `feedback_written()`

Marks the attachments the draw with state s writes, after ordering them behind earlier draws' reads of them
(`vk_exec_feedback_war`); sampled: what this draw itself samples in the feedback layout, which a later write must wait
for in turn (a draw reading and writing one target is the game's own feedback loop, ordered by region as it is).

### `tex_info_lookup()`

- `TD`:
  - `uint8_t reduced`: the image was made without its top levels and the resource did not mind (`GfxShaderRes.exact`).

Sampled-texture descriptors per (handle, resource type, separate), kept while nothing they derive from changed: the
slot's resolution and texture entry (`slot_still`) and the samplers (`vk_sampler_gen`); the layout is set again on every
hit from the images in the feedback layout (`in_fbl` of the image kept with it). The first binding of a slot in a
submission (`vk_texture_epoch`) still makes the slot lookup for its storage check; a re-upload writes the same image, so
the descriptor stays. Draws bind the same textures thousands of times a frame (15,000 lookups a frame in the monsters
fight); a hit skips the slot lookup, the view key with its hash probe and the sampler lookup. Storage images, which note
their binding per submission, are not kept. Direct-mapped; texture lock held. `NX2SARIA_TEX_MEMO=0` turns it off;
`=check` computes every hit again and stops on a difference.

- `uint32_t gen = vk_texture_slots_gen(), ep = vk_texture_epoch(), sg = vk_sampler_gen()`: before any lookup.
- `if (m->epoch != ep)`: the slot's first binding in this submission: only its storage check is due.
- `uint64_t tok = token_of(h)`: before the lookup.
- A lookup that made the texture whole again changed the slot's resolution after the stamps above were read: the entry
  never matches, and the next binding fills it.

### `direct_blank()`

Returns whether the image bound was made without its top mip levels (texture detail, `VkTexImage.skip`). For the flight
recorder: a draw's own binding of slot id resolved to no view, so the draw samples the stand-in. The pool's entries are
noted where they change (`pool_note_blank`); a binding written into a draw's own set is not, and it keeps what it got.
Noted once per slot and frame, for the first 256 slots of a frame. Texture lock held.

### `tex_info_full()`

- A shader that reads this binding's texels by position, or asks for its size, needs every level of the texture where
  the game put it (`runtime/gfx/gfx_shader.c` `reflect_exact()`): one made without its top levels is made again.
- A depth texture sampled without a compare reads one texel on the Switch, whatever the sampler's filter: blending
  neighbouring depths puts reconstructed positions in mid-air at object edges (shadow masks).

### Texture and sampler pools (bindless)

Shaders that take texture handles from memory (TEX.B and kin) index the whole texture pool: the translator declares
arrays of `VK_POOL_TEX` images and `VK_POOL_SAMP` samplers (index = handle & 0xfffff and handle >> 20), which
`runtime/vk/vk_pipeline.c` `spirv_pool_rebind()` moves to set 2, one image binding per view kind (`runtime/vk/vk.h`).
One set serves every such pipeline: entry id of binding k holds texture slot id viewed as kind k (a stand-in when the
slot is empty or cannot be viewed so), entry id of binding `VK_POOL_KINDS` sampler slot id. `pool_sync` rewrites the
entries whose slot changed (`vk_texture_slot_token`) and fills the kinds a new pipeline asked for; with
update-after-bind (descriptorIndexing) a write never invalidates a command buffer being recorded. Each submit's first
sync also looks every registered slot up once, the freshness check a bound texture gets (`vk_texture_by_id_locked`). The
set is one of a ring of `POOL_SETS`, one per submission: on the Switch the GPU reads the pool as it goes, but it runs a
submission right after `nvnQueueSubmitCommands`, so what it reads is the pool as of about the submit. Here the GPU runs
a submission several milliseconds later, and Ocarina registers slots for its next submission right after submitting:
with one set, an in-flight submission sampled a texture registered for the next one, the lit materials of distant
characters read the wrong texture as their light or probe data, wrote NaN and bloom spread it (white dialogue frames, a
fifth of them; none once each submission waited for its GPU work, `NX2SARIA_SUBMIT_FINISH`). A submission's first
`pool_sync` takes the next set (`pool_take_set`), waits for the GPU only if that set's last submission is still running,
and brings it up to date from the change log (every entry written since the set was last current; the whole set when the
log has wrapped). Writes within a submission go to its own set at once, until a part of the submission goes to the queue
early (`runtime/vk/vk_exec.c` `maybe_split()`): that part may read any entry of the set, so the set is sealed
(`vk_exec_pool_split`) and the next sync takes the following set as if a new submission began. Writing into it instead
updated descriptors a pending command buffer reads, which Vulkan does not allow: the part sampled textures registered
after it went, and on RADV a texture whose memory was mapped after the part's submit is absent from that submit's page
tables, a GPU page fault on the 128x128 probe pass of Kokiri Forest's first frame (RADV's `radv_wait_for_vm_map_updates`
hid it, as did `NX2SARIA_SPLIT_DRAWS=0`).

- `PoolSlot`: Per texture slot, what `pool_fill` keeps between refills: the descriptor hash of the registration it was
  read under, and the lookup memo stamp its entries were last written from (0: none) with the kinds that write covered.
- `POOL_LOG`: entries of the change log: binding * `VK_POOL_TEX` + id, one per write.
- `PL`:
  - `VkDescriptorSet set`: the current submission's (`sets[cur]`).
  - `uint64_t set_fence[POOL_SETS]`: the timeline value of the last submission that bound it.
  - `uint64_t set_synced[POOL_SETS]`: the change-log position up to which it holds every write.
  - `int nsets, cur, all`: all: the first fill, written to every set.
  - `uint64_t cur_fence`: the submission `sets[cur]` serves.
  - `uint32_t *log`: `POOL_LOG` entries.
  - `uint64_t logn`: writes so far.
  - `uint64_t *last`: per entry: the log position of its newest write (catch-up writes it once, there).
  - `uint64_t n_walk[2], walk_visit[2], walk_refill[2], walk_ns[2]`: dirty walks: at a submission's start, within one.
  - `uint64_t n_visit`: slots the dirty walks looked at.
  - `uint64_t n_gwalk, n_gskip, n_gskip_slots, n_grep`: `POOL_GROUP`: groups walked, left alone and their slots, first.
  - `uint64_t n_gchk_writes, n_gchk_groups`: members looked up for the memo.
  - `uint64_t n_gchk_writes, n_gchk_groups`: `POOL_GROUP_CHECK`: writes the groups left alone got all the same, and in
    how many.
  - `uint64_t n_gchk_pending, n_gchk_bad`: of members registered since their last fill (not counted above); this
    group's.
  - `uint64_t n_gnouni, n_gchanged`: groups walked member by member: not known to hold one resolution, first member
    changed.
  - `int gmode`: `pool_group()` as of the last sync.
  - `uint64_t n_by_reg, n_by_addr`: refills whose token changed by a registration, by an address generation only.
  - `uint64_t full_ns, n_full`: the walks over every slot (a submission's first sync) and their time.
  - `uint64_t ep_ns, n_ep, n_ep_ent`: `POOL_FAST`: a submission's freshness check of the noted entries instead.
  - `uint64_t unres_ns, n_unres_walks`: of it: the walks over the slots without an entry.
  - `uint32_t unres_mg`: `mem_map_gen` of the last such walk.
  - `int unres_more`: it left a lookup for the creation budget: the next submission walks again.
  - `PoolSlot *ps`: `[id]`.
  - `uint8_t *unres_on`: `[id]`: its last fill resolved to no entry (`POOL_FAST`: retried each submission).
  - `uint8_t *unres_in`: `[id]`: the slot is in unres (once: `pool_unres` never lists it a second time).
  - `int *unres, nunres, cunres`: those slots (some may have left: `unres_on` clear, dropped by the next walk).
  - `uint32_t *ent`: `[id]`: the entry its last fill resolved to, held there (`pool_hold`); 0: none.
  - `uint64_t dirty_pos`: `vk_texture_dirty_slots_locked` position (0: the next sync walks every slot).
  - `int ready, uab, sealed`: sealed: a part of the current submission already reads `sets[cur]`.
  - `int retry`: the last sync left slots to the creation budget.
  - `uint64_t frame, frame_ns`: the frame of the last sync, and the time its syncs spent making textures
    (`pool_create_ns`).
  - `uint64_t n_frame_stops, n_defer_skips`: syncs that created nothing for the frame's time, lookups they skipped.
  - `uint64_t *tok`: per texture slot: `vk_texture_slot_token` it was filled under (0: stand-ins).
  - `VkImageView *view`: [kind * `VK_POOL_TEX` + id]: what the entry holds.
  - `VkSampler *samp`: `[id]`.
  - `VkBuffer marks`: pool marks (`vk_exec_pool_marks`): the device buffer the marked shaders write, read-back copies in
    a ring.
  - `uint32_t marks_since`: submissions that recorded work since the last copy.

### `pool_limits()`

The device's limits for the sampled images of the pool's set: in one set, and in one shader stage of a pipeline layout
(the pool's bindings are visible to every stage, and a layout adds the bound textures of set 0 to them). The
update-after-bind limits apply when PL.uab, the plain ones otherwise. `*set` and `*stage` are 0 when unknown.

### `pool_layout_make()`

- `vkGetPhysicalDeviceFeatures2(c->phys, &f2)`: `create_device` enables every supported feature.
- The pool needs `VK_POOL_KINDS` x `VK_POOL_TEX` sampled images in its set, whatever the game uses: a translated shader
  indexes the whole array. A device whose limits are below that is told in the log before the layout is asked for; a
  driver that accepts the layout anyway is let go on, one that refuses it ends the start with the limit named.
- `uint32_t nb = VK_POOL_KINDS + 1 + (marks ? 1u : 0u)`: the marks buffer at `VK_POOL_MARK_BINDING`, last.
- `if (marks)`: written once, before any set is bound: no update-after-bind.
- `POOL_SETS` sets, fewer where the device has no room for them (one at the least, as before).

### Pool marks

(`NX2SARIA_TEX_DISUSE`; `runtime/vk/vk_pipeline.c` `spirv_pool_mark()` writes them, `runtime/vk/vk_texture.c` "disuse"
reads them) A device buffer of a uint per texture slot at `VK_POOL_MARK_BINDING` of every pool set: a marked shader
stores 1 into the slot of every pool texture it samples. Every `MARKS_EVERY` submissions that recorded work, the end of
the submission copies the buffer into a host-visible read-back buffer and clears it; a copy is in the submission that
signals the timeline value, so once `vk_sync_fence_done` reaches that value the copy is complete, and a later submission
hands it to `vk_pool_marks_read`. Up to `MARKS_RB` copies may be in flight; with none free, the marks stay and the next
submission tries. The buffer starts with whatever the memory held: a stray mark only keeps a texture one read longer,
and `runtime/vk/vk_texture.c` waits for two reads before it evicts by disuse.

### `pool_marks_make()`

- Two uints past the slots: the spare that a fragment shader's marks of accesses it did not reach go to, never read, and
  the overflow word, read with the slots (`VK_POOL_MARK_OVERFLOW`; `runtime/vk/vk_pipeline.c` `spirv_pool_mark()`).
- `__atomic_store_n(&PL.marks_ready, 1, __ATOMIC_RELEASE)`: the recording thread reads the rest after this.

### `pool_marks_release()`

`vk_exec_draw_shutdown`: a probe ends with the device idle.

### `vk_exec_pool_marks()`

- `while (PL.rb_n)`: the copies the GPU completed, in submission order.
- The shaders' stores before the copy; the copy's read before the clear.
- `VkBufferCopy region = {0, 0, (VkDeviceSize)(VK_POOL_TEX + 2) * 4}`: the slots, the spare and the overflow word.
- The clear before later shaders' stores, and the copy visible to the host.

### `pool_kind_type()`

The translator's sampler type (`GfxShaderRes.type`) of pool kind k.

- `static const uint16_t dim[4] = {1, 3, 4, 5}`: 1D, 2D, 3D, CUBE.

### `vk_exec_pool_loading()`

- `pool_loading`: replay: a sync left an area load's worth of slots, and they are not made yet.

Texture entries a pool sync may create (images and uploads), the rest at the next sync (`NX2SARIA_POOL_CREATE_BUDGET`,
default 16). The sync holds the texture lock throughout, and with 64 streaming in new textures made frames of 20-40 ms
with the replay and other threads waiting on that lock (24 ms of holds per slow frame in a dense area); 16 brought that
to 5 ms with no pop-in a player could see. The slots it leaves keep their stand-in until the next sync. An area load is
not streaming. It leaves thousands of slots at once (4,925 in the load measured below), and at the streaming budget they
came in over a hundred frames and more: the first seconds out of a load screen showed the ground, the walls and the
buildings as their black stand-ins. So while a sync leaves `NX2SARIA_POOL_LOAD_BACKLOG` slots or more (default 512; 0:
never) the pool is loading: the count budget is off, a frame's refreshes may take `NX2SARIA_POOL_LOAD_MS` (default 50),
and the presents of those frames are left out so the picture before the load stays up
(`runtime/vk/vk_backend.c` `frame_hold()`, `vk_exec_pool_loading`) until fewer than `POOL_LOAD_END` slots are left. The
load screen lasts those few frames longer and the area appears whole.

### `pool_create_ns()`

The refresh runs before every draw that binds the pools, not once per submission, so the count above held for each draw:
an area load made 4,925 entries (1.7 GB of uploads) in one 430 ms frame, and while slots were left over every draw
walked the whole table and looked each of them up again (key and hash), about 100 ms of that frame on its own. Once a
frame's refreshes have spent `NX2SARIA_POOL_CREATE_MS` (default 6; 0: no limit) making textures, its later ones create
nothing, and skip the slots left earlier in the frame (`pool_defer`) instead of looking them up again; the first refresh
of each frame keeps its count budget. The slots left keep their stand-in, a few frames longer in a large load. The time
counted is what the lookups spend making entries and uploading them the first time (`vk_texture_pool_make_ns_take`). It
was the whole refresh, with its check of every entry the pools hold: in Kakariko that check alone takes 0.8 ms a
submission, six submissions a frame, so most of the 6 ms was gone before a texture was made, and the frame's later
refreshes made none. The game moves about thirty of its textures to new storage every four or five frames there; the
ones a refresh left were on screen as their grey stand-ins for a frame (three lamp posts in one recorded frame), 87
refreshes in 600 frames with 1,090 slots left between them.

### `pool_left_add()`

- `pool_defer`: per slot: 1 + the low bits of the frame a refresh left it in.

The slots a refresh left (`pool_refill` failed: the creation budget or the frame's time was spent), so that the next
refresh of the same submission looks up these and the changed slots (`vk_texture_dirty_slots_locked`) instead of walking
the whole table again. While an area loads, slots are left at every refresh, and each refresh walked the table: about
67,000 slot reads and 1.65 ms of the replay per submission in Kokiri Forest on an RX 580 (2026-11-11), with six
submissions a frame. `pool_left_all`: the list overflowed (or nothing tracks it yet): walk the table.

### `pool_write()`

An entry whose wanted value PL.view / PL.samp already hold: into the current set, and into the log for the others (the
first fill goes into every set).

### `pool_entry_to()`

Writes entry e (binding * `VK_POOL_TEX` + id) of set as PL.view / PL.samp hold it now.

### `pool_take_set()`

- `pool_current()`: A submission's first pool use: the next set of the ring, once the GPU is done with it. 1 when it
  switched; `*from` is the log position the set holds writes up to (`pool_catch_up` brings it level once `pool_sync` has
  refreshed the entries: a view written into a set must be alive, and the refresh is what replaces the views of
  destroyed textures).
- `pool_takes`: sets taken (`vk_pool_takes`): counted once the take's `pool_sync` is done.

- A sealed set stays bound while it is current: reading it is what the pending part does too.

### `vk_exec_pool_split()`

- `if (PL.cur_fence != fence || PL.sealed) return 1`: no part of this submission has the current set unsealed.
- `pool_take_set` moves to the next set of the ring: it must not be bound by this submission's earlier parts too.

### `pool_catch_up()`

- `pool_memo_hits`: `pool_fill`: slots that shared another slot's lookup (defined with `pool_memo` below).
- `pool_memo_same`: of those, slots whose entries held the shared views already.

The current set gets the entries written into other sets in [from, to) of the log, as PL.view / PL.samp hold them now.

- `if (to - from > POOL_LOG)`: the log has wrapped since: every entry.
- Each entry once, at its newest write: an entry written again later (also by this submission's own sync, which wrote it
  into this set already) is skipped at the older positions.
- `if (PL.switches % 3600 == 0)`: diagnosis: what invalidates the slots
  (`runtime/vk/vk_texture.c` `vk_texture_bucket_stats()`).

### `shader_skipped()`

Entry id of every kind in mask: the slot's view of that kind or the kind's stand-in. Texture lock held. 0: the slot's
texture was left to a later sync by the creation budget (`pool_sync`). `NX2SARIA_SKIP_SHADERS=<hash>,...`: draws and
dispatches whose shader's SPIR-V has one of these FNV-1a hashes (the "vs"/"fs"/ "cs" hashes of the frame dump's
passes.txt) are skipped (diagnosis: which pass makes an effect).

- `uint64_t h = gfx_shader_spirv_fnv(sh)`: as `runtime/vk/vk_dump.c` `shader_hash()`.

### `pool_on()`

`NX2SARIA_POOL=0`: draws and dispatches of pipelines that index the pools are skipped (A/B against bindless shaders).

### `pool_fast()`

- `pool_stamp`: the last stamp given out.

Slots that resolve alike share one lookup (`vk_texture_slot_khash_locked`): Ocarina keeps ~400 descriptors of each
transient render target registered over its storage, and every finalize and initialize of that target changes their
tokens, so a refill looked up the same texture ~400 times, ~15,000 lookups a frame in Kokiri Forest. An entry holds the
views one slot got, for the slots with its hash, address generation and entry generation. Its stamp changes only when
one of its views does (`NX2SARIA_POOL_FAST`, default on): a slot whose entries were written from the entry at the stamp
it has now (`PoolSlot`) holds those views already, so a refill that finds the entry so compares and writes nothing. That
is most refills: a transient render target released and taken back moves the address generation of its storage, ~1,300
slots in Kokiri Forest, and every slot resolves as before (18,500 of 18,800 refills a submission shared a lookup, each
comparing up to 16 views a quarter megabyte apart: 1.9 ms).

ab: in the B blocks.

### `pool_hold()`

Slot id's last fill resolved to the entry ent (`vk_texture_pool_note_locked`; 0: none). `runtime/vk/vk_texture.c` counts
the slots that hold each entry: the entries held are the ones the walk over every slot reached with a lookup in each
submission, and the check by list leaves their use stamps from that count (`vk_texture_pool_hold_locked`).

### `pool_unres()`

Slot id's last fill resolved to no entry (got 0) or to one: the slots without one are looked up again each submission
(`pool_sync_entries`), as the walk over every slot did: an entry their storage could not have then may be made now, and
its address generation then refills them. A slot that resolved keeps its place in the list until the next walk drops it;
when it stops resolving before that walk, that place serves again. Listing it a second time made the list grow by one
entry per such change (160,000 entries for the 32,768 slots after a minute in the Market), and every 8th submission's
walk over it then held the texture lock for 15 to 30 ms.

### `pool_view_blank()`

For the flight recorder: v is one of the stand-in views.

### `pool_no_3d()`

For the flight recorder's blank-slot events: 16384 when no pipeline made so far reads the pool as a volume (kind = depth
* 8 + dim * 2 + arrayed, `runtime/vk/vk.h`), so a slot that holds a volume has a stand-in in every kind that is read.

### `pool_note_blank()`

- `pool_blanked`: the slots that lost their picture since the last `pool_blank_check`.

Slot id's entry of a kind went from the view was to now: noted when a texture's view gives way to the stand-in, which
every draw of the submission that samples the slot then reads (the set is written with update-after-bind).

### `pool_blank_check()`

A submission's first sync, before it changes anything: the slots that lost their picture during the submission before
and have none in any kind now went to the queue with their stand-ins. Texture lock taken here.

### `pool_slot_shown()`

Whether slot id's entry of a kind in mask is a texture's view now (`vk_texture_pool_shown_locked`).

### `pool_fill()`

- `if (tok)`: the descriptor hash changes only with a registration (the token's upper half).
- `ps->stamp = 0`: its entries match no memo entry, unless the one below.
- `if (m && ok)`: the generation after the lookups: an entry they created is part of what they resolved.
- A view that changes gives the entry a new stamp; one kept outside the mask keeps its value, so a kind that comes back
  with another view is a change too.

### `pool_sync_draw()`

A chunk of a parallel group (`runtime/vk/vk_exec.c` "replay workers") binds the set as `vk_exec_draw_group_begin` left
it, up to date when the group began; a chunk that still finds no set of this submission takes it in turn, under
`pool_mt`. Elsewhere the replay syncs the pools at every draw that binds them, as before, under `pool_mt` past a group
whose chunks still record (`NX2SARIA_REPLAY_PIPE`): the set is the submission's, read by the GPU when the part runs.

### `vk_exec_draw_group_begin()`

- `if (vk_ctx()->has_mesh_shader && !vk_ctx()->mesh_fallback) mesh_ring_make()`: the chunks' indirect mesh draws.
- The set this submission took, or its first one when it has none yet: taken by a chunk instead, that chunk waited for
  every earlier chunk of the group to be done (`pool_sync_draw`), and so did each chunk that reached its first draw
  before one had taken it, so the first group of a submission recorded one chunk at a time (Hyrule Field: 2.4 ms for 209
  draws where a later group of 784 took 0.8).
- `int lk = vk_exec_shared()`: earlier groups may still record (`NX2SARIA_REPLAY_PIPE`).

### `pool_sync()`

- `if (took && flight_on) pool_blank_check()`: what the submission before went to the queue with.
- `int switched = pool_take_set(ex, &from)`: every submission its own set, before the shortcut.
- `if (PL.sealed) return`: current when checked: a change since waits for the next sync, which takes a new set.
- `vk_texture_pool_make_ns_take()`: what other lookups left: only this sync's making counts for the frame.
- `pool_catch_up(from, pre)`: what the other sets got since; this sync's own writes went into it already.
- A set taken and up to date (with one set, the refresh above wrote into it directly), counted only when this sync left
  no slot behind: once the frame's creation time is spent, `pool_refill` skips a slot it left earlier in the frame and
  `pool_sync_entries` may return before the walk, and such a slot keeps its old view in this set with nothing in the
  log, so the takes after it would not carry the change into the other sets. After a counted take, every slot change
  made before it is in this set and in the log; `runtime/vk/vk_texture.c` `retire_sweep()` frees a retired image only
  after more than `VK_POOL_SETS` of them, so no set of the ring still holds its view.

### `pool_refill()`

A slot whose token changed: filled, or noted as left (`pool_defer`). With the frame's creation time spent (spent), a
slot left earlier in the frame is not looked up again. A left slot holds the stand-ins (its lookup resolved to nothing),
and a fill clears the note: kept, a slot left and then filled in the same frame was skipped later in that frame with the
filled texture's view still in the set, also after that texture was destroyed (a released one dropped, one evicted), and
a shader sampling the slot read freed memory.

### `pool_dirty_slot()`

`vk_texture_dirty_slots_locked`: a slot whose token may have changed.

### `pool_current()`

Every entry as the slots, kinds and samplers are now (`pool_sync_entries` has nothing to write).

### `pool_group()`

`NX2SARIA_POOL_GROUP=1` (default), 0, ab: a dirty bucket's slot groups (`runtime/vk/vk_texture.c`) are walked whole. The
members of a group resolve alike, so once a walk of the group brought every member to the current resolution (every fill
done, an entry for each) they all hold the same views, and later the group is left alone when its first member still
holds the current resolution: its token did not change (its views are what that token resolves to), or its refill wrote
nothing. A slot that joined since is at the tail and refilled on its own (its registration's entry on the dirty ring).
The members keep their older tokens: their views are what those tokens resolved to, and what the current ones do.
(Comparing the memo's stamps instead left half the groups to their members: the memo's entries are shared between
hashes, so the same views came back under a new stamp.).

### `pool_group_check()`

`NX2SARIA_POOL_GROUP_CHECK=1`: a group left alone is walked member by member all the same, and the writes that walk
makes are counted (none when the rule holds; the pool log's slot groups line), but for the members registered since
their last fill: their registration's entry on the dirty ring refills them in the same walk (counted apart).

- `if (on < 0) on = rt_knob("POOL_GROUP_CHECK", "0") == 1`: a diagnosis: no settings row.

### `pool_dirty_group()`

- `if (!same)`: the first member's refill: nothing written, an entry found, its token taken.
- `int r0 = d->retry, uni = 1`: member by member: then all hold the current resolution, unless a fill was left.
- `if (id >= d->n) continue`: outside the pool.
- `if (chk)`: the check: a member registered since its last fill is refilled by its own entry anyway.

### `pool_sync_entries()`

- `int spent = pool_create_ns() && PL.frame_ns >= pool_create_ns()`: this frame creates nothing more.
- `if (spent && PL.ready && used == PL.kinds && gen == PL.gen && ep == PL.epoch && sg == PL.sgen) ...`: left slots only.
- `vk_texture_pool_ctx(1)`: these lookups are the pool's own: a texture evicted by disuse keeps its stand-in.
- `if (!PL.ready)`: every entry a stand-in first: partially bound arrays are still read out of range by guests.
- `int retry = 0`: slots whose texture the creation budget left: filled again by the next sync.
- Only a slot change since the last sync (no new kinds, no new submission): the slots that changed
  (`vk_texture_dirty_slots_locked`), not the whole table. With `NX2SARIA_POOL_FAST` (default on) a new submission too:
  its freshness check goes through the entries the slots resolved to and the slots that resolved to none
  (`vk_texture_pool_fresh_locked`, `pool_unres`), not every slot.
- `if (grp && !PL.gmode) vk_texture_groups_unstamp_locked()`: members were refilled one by one meanwhile.
- With slots left by an earlier refresh (the creation budget or the frame's time spent): those (`pool_left`) first, then
  the changed slots, instead of the walk over every slot. While an area loads, slots are left at every refresh, and each
  one walked the table: about 67,000 slot reads and 1.65 ms of the replay per submission in Kokiri Forest on an RX 580
  (0.7.1, 2026-11-11), six submissions a frame. `pool_left_all`: no list kept yet (until the first walk): walk.
- The slots without an entry are over storage the guest has not mapped (stale descriptors, 1,160 in Kokiri Forest):
  looked up again when the mapping changed, while a lookup is left for the creation budget, and every 8th submission for
  the rest (an entry whose creation failed, an image at their address that took GPU content since: `slot_resolve`'s
  alias rule).
- `if (r)`: resolves now (an entry it made changes its token: refilled), was refilled since, or.
- `PL.unres_on[id] = 0`: resolves to none until its token changes (the refill lists it again).
- `for (int id = 0; id < n && !walked; id++)`: slots past n were never registered: stand-ins since the first sync.
- `vk_texture_by_id_locked(id, NULL, NULL)`: the submit's freshness check.
- `vk_texture_groups_unstamp_locked()`: the walk over every slot refilled members one by one.
- `retry = (int)pool_nleft`: every slot left, by this refresh or an earlier one not filled since.
- Retry counts every slot left (`pool_left`: this sync's and the earlier ones' not filled since).
- `stutter_load()`: the stutter map counts the frames around it as an area load's.

### Depth twins: colour images under a compare sampler in a compute shader

The game's compute passes sample an `R32_SFLOAT` texture through a shadow sampler. Maxwell compares any single-channel
float or unorm texel; Vulkan only formats with `SAMPLED_IMAGE_DEPTH_COMPARISON` (VUID-vkCmdDispatch-None-06479), which
AMD's drivers do not give `R32_SFLOAT`, so the result there is undefined. Such an image gets a depth twin of the same
size and bits (`R32_SFLOAT` -> `D32_SFLOAT`, `R16_UNORM` -> `D16_UNORM`), copied through a buffer right before each
dispatch that samples it (a dispatch is outside any render pass), and the dispatch samples the twin. Draws keep the
colour image: a copy cannot go inside their pass. `NX2SARIA_DEPTH_TWIN=0`: off.

- `twin_dispatch`: `op_dispatch*`: the resources being bound are a dispatch's, on this thread (the replay workers record
  draws meanwhile); 2 when the dispatch is a network program's (`uses_tensor_ops`; `net_binding`).

### `twin_free()`

Completion thread, after the last submission that may sample it.

### `twin_buf_free()`

Completion thread, after the last submission that may copy through it.

### `twin_format()`

- `static int compares[2] = {-1, -1}`: per source format: the driver compares it as it is.

### `twin_update()`

The twin of img holding img's current texels, the copies recorded into `ex->cb`; NULL when img needs none or it cannot
be made.

- `if (bytes > twin_buf_cap)`: one copy buffer, grown; the old one goes after the submission being recorded.
- `if (t->img)`: the submission being recorded may still sample the replaced twin.
- The source's earlier writes and the twin's earlier readers before the copies; the twin's old texels are dropped.
- The twin's texels before the dispatch; the source's next writers and the buffer's next copy after these copies.

### `net_binding()`

What a network program run natively (`NX2SARIA_NATIVE_TENSOR=1`, `native_tensor`) binds at slot h, before its dispatch.
Its input picture: the game's scene upscaler samples the post pass's picture through an RGB9E5 texture that lies at the
scene target's address. On the console the two are one memory, so the words the post pass stores through the target's
integer view are that texture's texels. Here they are two images (`raw_9e5_twin`), and the RGB9E5 one holds what it was
given when it was made: an older picture, or the target's R11G11B10F words read as shared-exponent texels, which is
noise up to 65408 that the half-float network turns into infinities. The stand-in reads the target's image for that
reason (`untranslated_dispatch_fallback`); the network's own code samples the texture, so the words are copied into it
here (the same texel size: a copy keeps the bits), each written picture once. Its output: an image made without storage
use cannot be written, and the dispatch then writes a dummy; logged, as the picture is then lost without another sign.
Texture lock held.

- `if (o == t) return`: written as native texels, later than the target was.
- The target's earlier writes and the texture's earlier readers before the copy, the copy before the dispatch.

### `tex_info()`

The image descriptor of slot h; in a dispatch, a compare-sampled colour image the driver cannot compare is swapped for
its depth twin; in a pass larger than it, the scene's light target, once DLSS scaled it this frame, for DLSS's output
(`runtime/vk/vk_dlaa.c`).

### `alloc_set()`

- `if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY)`: once more, as scratch.

### `support_in()`

What `support_fill` reads: viewport size, point size.

### `pcss_params()`

`NX2SARIA_PCSS` (percent, 0 = off): the pcss field of the support buffer, which translator patch 0010 reads in every
explicit-LOD compare on a 2D array texture (the sun cascades). x: penumbra width in UV per unit of light-space depth
between receiver and blocker (0 keeps the game's single compare), y and z: the narrowest and widest disc radius, w: the
blocker search radius, all in UV of the cascade map. Radii are in UV, so they hold at any `NX2SARIA_SHADOW_SCALE`.
`NX2SARIA_PCSS_K` overrides the depth-to-penumbra factor at 100 percent, for tuning.

### `shadow_spread_params()`

The `shadow_spread` field of the support buffer: the factor translator patch 0011 applies to the lighting shaders' sun
shadow filter offset (element 56 .xy of their lighting block), 1 / `NX2SARIA_SHADOW_SCALE`. The game spaces its four
compares for its own shadow map texels; with maps `NX2SARIA_SHADOW_SCALE` times larger, the same offset in UV spans that
many more texels and the near cascade shows four separate copies of each edge. Dividing by the scale keeps the game's
spacing in texels. The offset is in UV of the shadow map, so the output resolution plays no part. The scale is read
once, as `runtime/vk/vk_texture.c` `shadow_scale()` reads it: the maps keep the size they were made with.

### `support_fill()`

- `memcpy(b + 1392, &point_size, 4)`: `point_size` (translator patch 0008): vertex shaders that write none output it.
- `memcpy(b + 1408, pcss_params(), 16)`: pcss (translator patch 0010): soft sun shadows in the cascade compares.
- `memcpy(b + 1424, shadow_spread_params(), 16)`: `shadow_spread` (translator patch 0011): sun shadow filter width.
- `for (int i = 0; i < 73; i++) memcpy(b + 192 + 16 * i, &one, 4)`: render scale 1: the host renders at guest size.
- `if (vk_hdr_armed())`: read by the game's tonemap only, once `runtime/vk/vk_hdr.c` `tonemap_splice()` has grown its
  block past the translator's.

### `bind_stats_on()`

`NX2SARIA_BIND_STATS=1`: bindings and descriptors per draw of each stage (Task 2.2's choice of the pushed stage), and
how often a draw repeats the previous one, logged every 20000 draws: pushes recorded and skipped, allocated sets written
and reused, stage texture-type recomputations (`answers_for`), draws with the previous draw's program, bound state and
textures (shaders and pipeline would be the same) and those that also have its buffers and viewport (all descriptors
would be the same). BS is declared beside DS.

### `repeat_stats()`

The bound state a draw reads, compared with the previous draw's (`NX2SARIA_BIND_STATS` only).

### `push_skip_on()`

`NX2SARIA_PUSH_SKIP=0`: every graphics draw pushes its set again.

### `bind_skip_on()`

A graphics draw whose descriptor inputs equal the previous graphics draw's in this command buffer skips building its
sets: every write would be the same, the push would be skipped and the allocated set reused without a bind (about a
quarter of field and monsters2 draws, `NX2SARIA_BIND_STATS`). The inputs, compared and stored in place, are what
`bind_resources` and its helpers read beyond the pipeline info's fixed bindings and Z's stand-ins:

- The info (bindings, layouts), the program and its serial (the compiler-constant range of UBO slot 1);
- `vk_texture_slots_gen`, `vk_texture_epoch` (a slot's image, view and once-per-submit storage check), `vk_sampler_gen`;
- `vk_mem_gen` (buffer and offset of a span, mirror creation and per-submission sync);
- The images in the feedback layout (the sampled layout);
- Per binding: the support inputs, c0's storage table and base instance, the bound uniform and storage ranges, the
  texture and image handles (`RsList`; compared only when a bind op ran since the last draw, `bind_gen`);
- Storage buffer addresses read from user uniform blocks (RsList.p, `ubo_ptr`): compared on every draw, since the
  block's contents change without a bind op.

The side effects of a build repeat as no-ops in the same command buffer and submission: c0 and support buffers kept in
B, `note_written`'s ranges, `vk_mem_gpu_wrote`'s write-backs, the per-submission mirror sync and dirty census note, the
views cached per submission. The census counts are added again. A build that copied guest bytes into transient memory
(an unaligned host range) is never skipped after: the copy may differ. `B.res_valid` says the sets bound hold the last
build; `vk_exec_draw_new_cb`'s memset clears it, and every build clears it until it ends. `NX2SARIA_BIND_SKIP=0` builds
every draw; `=check` builds every draw and stops when a draw with equal inputs gets other writes, census counts or
descriptor commands.

### `rs_list()`

- `if ((nd += cnt) > MAX_DESC) { ok = 0; break; }`: the build stops on it.
- Slot 1: the program (`bind_sig`); others: the zero buffer.
- `} else if (b->kind == GFX_RES_SSBO)`: every descriptor of the binding is the same range.
- `if (r->ptr || (!direct && ubo_ptr_res(r)))`: an address read from memory: `bind_sig` reads it again.

### `bind_sig()`

The signature of a graphics draw's descriptor inputs into RS; 1 when it equals the stored one. The bound-slot bytes are
compared only when a bind op ran since they were stored or the pipeline changed: otherwise they are the same. Check mode
compares them anyway and stops when the bind op generation said they could not differ but they do.

- `PUT(vk_mem_gen() + vk_usnap_gen())`: both only grow.

### `bt_now()`

The binding profile in the `NX2SARIA_TRACE=gfx` frame line reads the clock around every descriptor; without the trace
those reads would cost more than some of what they time, so they read 0. The phases `NX2SARIA_REPLAY_PHASES` times
(`vk_exec_op_draw` below); the binding's four are inside `PH_BIND`.

### Per-binding descriptor reuse

A graphics draw whose inputs differ from the previous draw's in one binding (the whole-draw skip, `bind_sig`, misses it)
used to resolve every one of its descriptors again: each uniform and storage range through `vk_mem_resolve` and its
mirror, each texture through the descriptor memo and the slot table, hundreds of thousands a frame in Kokiri Forest. PB
keeps, per pipeline info (the `PB_N` most recent, direct-mapped), each descriptor's last result together with the
bound-slot values it came from, and a descriptor whose values are unchanged takes that result instead of resolving.
Results stay valid while nothing else a resolution reads changes (`pb_globals`): the command buffer, the program, the
texture slot, epoch and sampler generations, the memory generation, the feedback-layout images and DLSS's stand-in
image. `vk_mem_gen` moves with every submission, so a result never outlives the submission whose mirror sync and
write-back notes it relied on; within one, repeating `vk_mem_use` for the same pool is a no-op, which is why it can be
left out. Only descriptors that read nothing but their bound slot are kept (`pb_key`): uniform slots 1 and 3-20, storage
buffers the shader only reads that a stage slot names, and sampled textures; written storage, c0, pointer tables,
storage images and the support buffer resolve every time. A reuse counts its census bytes again, and a resolution that
copied guest bytes into transient memory is never kept. Off while per-object motion replaces uniform bindings
(`mo_ovr_on`). `NX2SARIA_BIND_REUSE=0` resolves every descriptor; `=check` resolves every descriptor and stops on one
that resolves differently from its kept result with the same inputs. The entries (`PbEntry`, `DrawCtx.PB`) and the build
running (`DrawCtx.pb_e`) belong to the recording context.

### `pb_globals()`

Everything besides its own bound slot that a kept descriptor's resolution read.

### `pb_entry()`

info's entry, emptied when it held another pipeline's results or the globals moved.

### `pb_key()`

The bound-slot values descriptor k of binding b resolves from, when they are all it reads besides `pb_globals`.

- `return 1`: slot 1: the program's constants (`pb_globals`); others: the zero buffer.
- `if ((r->flags & 1) || r->ptr || r->cbuf_slot >= VK_GPA_CB_SLOT) return 0`: the table, the mesh fallback's buffers.

### `pb_take()`

Before descriptor j (descriptor k of binding b) resolves: 1 when its kept result serves, `*o` filled and its census
counted again. 0: resolve it, then `pb_keep`.

### `pb_keep()`

After descriptor j resolved to `*o`.

### `write_set()`

Writes allocated set s from its packed descriptors d: through the layout's update template, or with the write structs
`w[i0..i1`) when it has none.

### `gpa_reader()`

- `emu_d`: Writes the pipeline's sets from the bound state: set 0, and set 1 with independent layouts. Each is pushed
  into the command buffer, or allocated, written and bound; a graphics set whose contents equal the last one allocated
  for that set number in this command buffer is reused, and bound again only when another layout or a push came between.
  A graphics push equal to the last one at its set number, with no other layout or bind between, is not recorded. The
  descriptors go into one packed array d, a `VkDescSlot` per descriptor in binding order, which a set's update template
  reads directly (`VkPipeInfo.tpl`); write structs are filled only when a set has no template.

  set 0's descriptors of the last `bind_resources` build (mesh emulation).

A pipeline that binds the device address table reads what it names, mostly guest memory itself (pools without a mirror),
as the GPU runs: the submission's submit wait is the GPU's (`vk_mem_take_live`). Without this the staged mirror uploads
(`NX2SARIA_MIRROR_STAGE`) ended the wait once the replay had submitted, and the game wrote the next frame's instances
while the GPU still drew from them: the small grass and moss, which reach their instances through the table, flickered
and slid over the ground as the camera turned (RX 580, Kokiri Forest, 2026-11-11). Noted at every bind, before the bind
skip and the kept descriptors (`bind_sig`, `pb_take`), which leave `ssbo_info` out: noted only there, it was missed once
those had warmed up, and the flicker came back after a minute of play. the pipeline's binding list holds a table
binding.

- `int8_t g = __atomic_load_n(&info->gpa, __ATOMIC_RELAXED)`: the replay workers bind too.

### `bind_resources()`

- `cx->B.res_valid = 0`: until the build below ends.
- `uint32_t lo[2] = {0, 0}, hi[2] = {0, 0}, d0[2] = {0, 0}`: bindings [lo, hi) and first descriptor per set.
- `int need_w = 0`: a set without a template: fill the write structs.
- `int st = nvn_stage_of(b->stage), kind = 0`: kind: 0 buffer, 1 image, 2 texel buffer view.
- `if (!cx->B.sup_valid || memcmp(in, cx->B.sup_in, sizeof in))`: `support_fill` reads only these.
- A new buffer only when the contents changed.
- `for (uint32_t k = 0; k < cnt; k++)`: buffer textures are not replayed yet.
- `int locked = 0`: `tex_info` looks up with the texture lock held; kept descriptors (`pb_take`) need none.
- The context's memo (`TexMemo`) for sampled textures while other threads record (the lock is free otherwise), outside a
  dispatch's depth twins and DLSS's stand-in image, with the generations read before any lookup below.
- `uint64_t tok = m ? token_of(h) : 0`: before the lookup.
- `o[k].w[2] = (uint32_t)o[k].image.imageLayout`: zero the tail padding.
- `if (kind == 0) w[i].pBufferInfo = &o->buffer`: a `VkDescSlot` is as large as either info struct.
- An empty set 1 (a fragment stage without resources: depth and shadow draws) starts past the last binding, which the
  loop never reaches: `d0[1]` stayed 0 and set 0 was pushed with 0 bytes, so the driver read stale descriptors.
- Under the validation layer, a new graphics layout binds set 1 before set 0 is pushed or bound. The layer (SDK 1.4.363,
  `PipelineLayoutCompatDef::operator==`) takes any two `INDEPENDENT_SETS` layouts as compatible, so
  vkCmdPushDescriptorSetKHR keeps the push set it made for an earlier layout and drops writes to bindings that layout
  lacks (false VUID-vkCmdDrawIndexed-None-08114, then an access violation inside the layer). A bind at set 1 disturbs
  set 0 by its canonical id, which drops the layer's push set: the push after it gets a new one.
- `if (gfx && info->layout != cx->B.ds_layout)`: another layout: sets bound before may be disturbed.
- `if (s && i1 == i0)`: no fragment stage: set 1 is empty and unused.
- `if (set1_first)`: bound only to drop the layer's push set (comment above).
- `if (info->pool)`: set 2: the texture and sampler pools.
- `cx->RS.volatile_ = 1`: the pools may change under an equal draw: build the next one again.
- An equal next draw records nothing when every push set holds its push (`NX2SARIA_PUSH_SKIP` on) and nothing made the
  build unrepeatable; allocated sets are always bound to the one written last.
- `if (skip == 2)`: with the pipeline info equal (`bind_sig`), the packed descriptors determine every write.
- `if (same && was_valid) BS.bind_skip++`: the draws a skip would have spared, compared instead.

### Draws

### `base_viewport()`

- `VkViewport v = {x, y + h, w, -h, ex->depth_range[0], ex->depth_range[1]}`: the y flip: negative height.

### `draw_viewport()`

- `if (ex->mark)`: `NX2SARIA_PASS_TIMES`: how much of the pass the draws cover.
- `vk_dlaa_viewport(ex, &v)`: the scene's draws move by the DLAA jitter.

### `slope_bias()`

The slope-scaled depth bias of a draw. The term is the bias per unit of depth slope across one pixel of the target, and
a shadow map made k times larger on each side (`NX2SARIA_SHADOW_SCALE`, `runtime/vk/vk_texture.c` `shadow_scale_of()`,
`pass_scale`) has 1/k of the slope across each of its texels: the game's factor alone gives k times less bias than it
asked for on sloped surfaces, which is what shows as acne (stripes of self-shadow over slanted ground and walls). The
constant term counts the depth format's smallest steps, which do not change with the size, and keeps its value.

- `if (ex->pass_scale > 1)`: the log names each scale it meets, at most 8 times a run (replay workers record too).

### `record_fixed()`

- `NX2SARIA_DEPTH_BIAS_X` scales the constant term (diagnostic): NVN's units are tuned for the Switch's fixed-point
  depth buffer, and this back end draws into a 32-bit float one, where the same constant means a far smaller offset. If
  decals z-fight here, this multiplier says whether the offset is the reason.

### `bind_vertex_buffers()`

- A stream whose range is the one its bound buffer was resolved from, with no pool or mirror change since (`vk_mem_gen`,
  read before any resolve), resolves to that buffer again: most draws change one stream or none.
- Most draws change no stream at all: with every range as it was bound and no pool or mirror change, the loop below
  would take each buffer from B again and return at the compare after it.

### `foliage_shadow_cull()`

`NX2SARIA_FOLIAGE_SHADOW_CULL=<layer>[f]` (0 to 3, default 1; 3 turns it off): the foliage's mesh draws into the sun's
shadow cascades from this layer on drop one side of their cards ("f": the front faces, else the back faces). The game
draws them double-sided, and stacks of alpha-tested cards seen from the sun reached about 1,000 million fragments in one
draw of the 2.5 million texel far cascade, where half the cards still cover the same texels. Cull mode is dynamic state,
so this needs no other pipeline.

### `pipe_skip_mode()`

`NX2SARIA_PIPE_SKIP`: which draws are skipped, rather than waited for, while their shaders or pipeline build. strict
(0): only draws whose pass targets were all rendered in the previous presented frame; a pass with a target.

- That was not counts as one-shot and waits.

Any (1): also draws in a pass with at least one target rendered in the previous frame (a per-frame depth.

- Buffer or g-buffer with a colour target the frame before did not use: half-resolution effect buffers in fights).
- Passes with none (volumes, LUTs, shadow layers seen for the first time) still wait, and so do passes whose only.
- Recent target is the depth buffer while a colour target is rendered for the first time (item icon captures).
- Full-screen draws into such passes wait from their budget of 50 ms a presented frame. The default: a skipped.
- full-screen draw is a lighting, fog or tone mapping pass that leaves its target as it was, which shows as a black.
- Or unlit frame with the interface intact, and with the stage libraries in the driver's cache the wait is a.
- Millisecond (`runtime/vk/vk_pipeline.c` `lw_start()` keeps them there). One that is skipped once its budget is spent
  leaves a frame.
- That is not shown (`runtime/vk/vk_backend.c` `frame_hold()`); where that cannot hold the picture any longer, the draw
  waits on.
- (`FULLSCREEN_LAST_MS`).

Fullscreen (2): as any, and full-screen draws into such passes are skipped instead of waiting from their.

- budget. A full-screen pass whose stage libraries are cold in the driver cache took 86 ms on average (up to 194 ms).
- In a fight on an RX 9070: three frames in a row each spent the 50 ms budget, and some of the draws were skipped.
- After all. Skipping costs the pass for the frames until the pipeline is ready. It was the default until the.
- Libraries were kept warm.

Any other draw whose pipeline is missing waits for its fast link from a small budget per presented frame (`link_budget`)
before it is skipped.

### `writers_wait_on()`

`NX2SARIA_DRAW_WRITERS_WAIT` (default 1; 0 = off): a draw whose shaders write memory waits for its shaders and pipeline
as a one-shot draw does, whatever its targets. A draw into per-frame targets may be left out while its pipeline builds
because the game renders those targets again in the next frame (`pipe_skip_mode`). A storage buffer its shaders write,
or an image they store into, is no such target: the write is a side effect of this draw that later draws and frames
read, and with the draw left out that memory keeps what it held. So the draw takes the wait of a draw into targets that
are not per-frame (`prepare_draw_ex`: from the frame's wait pool, or `ONESHOT_WAIT_MS` with
`NX2SARIA_ONESHOT_BUDGET=0`), and is skipped only at the end of it. `draw_stats_tick`'s line counts the draws that
waited by this rule alone.

### `stage_reads_images()`

1 when a stage reads a texture or a storage image (`mfb_draw`, `MFB_ONE_TEXTURES`).

### `stage_writes()`

1 when the stage writes a storage buffer or stores into an image (`GfxShaderRes.flags`, `runtime/gfx/gfx.h`).

### `draw_writes()`

1 when a draw with these stages waits because it writes memory (`writers_wait_on`). A stage that is NULL writes nothing:
the program has none, or it is still being translated and has no reflection to ask yet.

### `frame_wait()`

`NX2SARIA_PIPE_LINK_MS` (default 6; 0 = off): how long the draws of one presented frame may wait, in all, for the fast
link of a pipeline that is missing, `LINK_DRAW_MS` at most each. A draw into a per-frame pass used to be skipped at once
and for as long as its pipeline built, also when its stage libraries were in the driver's cache and the link took under
a millisecond: geometry seen for the first time (ground, a wall, a character) was missing for a frame or more and then
appeared. With the libraries warm the wait ends within the budget and nothing is skipped; with them cold it runs out
after a few draws and the rest are skipped as before, so a burst of cold pipelines costs a frame this much and no more.
Measured on an RX 7900 XT, first walk through a village with no pipeline recorded: 2,349 draws skipped without the
budget, with the libraries warm in both runs. `NX2SARIA_PIPE_LINK_DRAW_MS` (default 3): the wait for one pipeline,
counted from its first miss and shared by all its draws (`prepare_draw_ex`), so a pipeline whose libraries the driver
has to compile costs its first frame this much and the frames after it nothing; it was 3 ms for each draw, in every
frame. A frame that has left a draw out already waits for no fast link either (`pending_draws`): it is held back or
shown without that draw whatever its other pipelines do. The 3 ms are just under what a pipeline takes with its two
shader libraries in the driver's cache: on a new install's first visits (RTX 5090, the libraries prepared, 67 pipelines
the install's list did not hold) 44 were usable under 4 ms after their miss, 17 after 4 to 8 ms and 6 later, and of 87
draws that waited 66 were skipped at the end of the wait, their frame then held back. 8 ms a pipeline and 16 ms a frame
were tried for that. A scripted walk through a village with no pipeline recorded (610 missed) had the picture stand
still 15 times against 50 and 15 frames over 25 ms against 17, but in play it felt worse to the player than the 3 ms it
replaced, in two sessions on the same install: a frame that takes 10 to 14 ms longer showed more than one that is left
out. The waits stay short; the settings take longer ones. `NX2SARIA_PIPE_FRAME_WAIT_MS` (default 50; 0 to 500): the wait
pool of one presented frame. Its full-screen draws and its one-shot draws (`NX2SARIA_ONESHOT_BUDGET`) wait for their
shaders and pipelines from it, each for what the ones before left, and are skipped once it is spent, so the pipelines of
one frame hold the replay thread this long in all and no longer (beside the fast links' own small budget, `link_budget`,
and `FULLSCREEN_LAST_MS`).

### `pool_ms()`

The milliseconds of pool not used up since t0, when the caller began to draw on it; `UINT32_MAX` without a pool.

### `mesh_whole()`

1 for a draw whose pipeline is a mesh pipeline without stage libraries, which gets no fast link (`MESH_WAIT_MS`). mesh:
the draw binds a mesh pipeline.

### `oneshot_ms()`

What a one-shot draw with the state s may wait for its pipeline from pool.

- `if (vk_pipeline_building_ms(s) > MESH_WAIT_MS) return 0`: a compile (`MESH_WAIT_MS`).

### Wait accounting

What the replay thread spent on draws that found their pipeline or a shader missing Counted from the miss to the draw's
bind or skip, the look-ahead scan included, by the kind of wait the draw was allowed; left: the draws skipped at the end
of it. `draw_stats_tick` logs the sums with the skipped draws, so a session's log says how much time the pipelines cost
on this thread and which rule spent it. writers: the draws into per-frame targets that took a one-shot wait because
their shaders write memory (`draw_writes`); they are among the one-shot kinds' counts.

- `FULLSCREEN_LAST_MS`: How long the full-screen draws of a frame wait on for their pipelines, in all, once the frame's
  wait pool is spent and the previous picture cannot be held any longer (`prepare_draw_ex`, `last_wait`). Every such
  pipeline has a fast link (`runtime/vk/vk_pipeline.c` `gpl_feedback()`), so the wait is for stage libraries: a few
  milliseconds from the driver's cache, and in the one case a player's session shows the draw had its pipeline 18 ms
  into this wait. It was 500 ms for each draw while a pipeline with a feedback loop waited for its whole optimised
  build; with the pool before it, a frame now waits under 100 ms for pipelines in all.
- `sm_on`: The previous draw's shaders, kept while its program, bound pipeline state, the vertex and fragment stages'
  first 32 texture and image handles and every slot's resolution (`vk_texture_slots_gen`, read unlocked: a registration
  racing with it is ordered after this draw, as `answers_for`'s own check would order it) stay the same: `answers_for`
  reads nothing else, so `shader_for` would return the same shaders. About 65% of field draws repeat them
  (`NX2SARIA_BIND_STATS`). `NX2SARIA_SHADER_MEMO=0` turns it off; `=check` looks the shaders up on every hit too, and
  every slot's texture type in `answers_for`, and stops on a difference. `tex_serial` (bumped by every texture and image
  bind) equal to the key's means the handles were not rebound since they were compared, so the 1 KiB compare is skipped;
  a rebind to the same handles still matches by the compare. The memo and its key (`MemoKey`) are `DrawCtx.SM`.
- `pm_on`: the memo is `DrawCtx.PM`.
- `vk_draw_skip_names`: Pass, shaders, pipeline, fixed state and resources for a draw. 0: the draw is skipped. ---- draw
  accounting: why draws were not drawn Every draw `prepare_draw_ex` turns away is counted by reason, vertex and mesh
  draws apart. The first draw of each kind skipped for each reason is logged with its program, and every
  `DRAW_STATS_FRAMES` presented frames one line sums up the draws issued and skipped since the last line (only when
  something was skipped). A black screen with draws skipped for "shader failed" or "pipeline failed" points at the
  translator or the pipeline; draws issued and still nothing on screen point at the shaders' results (`NX2SARIA_DUMP`).

### `draw_stats_tick()`

- `for (int k = 0; k < WK_N && o < (int)sizeof line - 320; k++)`: room for the two counts after the kinds.

### `vk_exec_pending_draws_take()`

The draws skipped since the last present because their shader or pipeline was still building: the picture of that frame
lacks them (a light, a wall, a whole lighting pass). `runtime/vk/vk_backend.c` `frame_hold()` reads it at the present
and keeps the previous picture on screen instead. The replay and its parallel chunks add to it; the present takes it, on
the presenting thread, which with the replay on the submitting threads need not be the one replaying: atomics.

### `draw_skipped()`

- `if (why == SK_PIPE_PENDING) perf_count(PERF_CNT_PIPE_SKIP)`: skipped draws only: a lookup that then waited is none.

### `skip_prog_wanted()`

`NX2SARIA_SKIP_PROG=<serial>[,<serial>...]`: diagnostics. Drops every draw whose program serial is listed, so something
on screen can be attributed to one program by elimination: pass the programs of the pass it appears in, then half of
them, and so on. Environment only, and nothing is skipped without it. Read once: a getenv on every draw scanned the
whole environment block and was a sixth of the replay thread's samples in a play profile.

### `prog_lead_on()`

`NX2SARIA_PROG_LEAD=1` (diagnostics): how long before its first draw the game made each program (`nvnProgramSetShaders`,
`runtime/nvn/nvn_api_obj.c` `nvn_prog_made_ns()`): the head start a build begun at that moment would have over one begun
at the draw, which is where shaders are looked up and pipelines built today. A line for vertex, mesh and compute
programs (their first dispatch) each, after every 200 programs first used.

### `prog_lead()`

mesh: 0 a vertex program's draw, 1 a mesh program's, 2 a compute program's dispatch.

mesh: a mesh draw (`op_draw_mesh`): the program's mesh and task stages stand in for the vertex stage, and nothing
vertex-related (vertex buffers, motion vector bookkeeping of vertex shaders) is bound or noted. 2: a mesh draw of the
mesh fallback (`mfb_draw`): the vertex stage is `DrawCtx.mfb_cur_vs`, in an ordinary pipeline without vertex input.

### Mesh fallback

A list's depth-only draw is held while its colour draw waits for a pipeline The game draws a culled list of grass or
leaves twice from one indirect record: into the depth target alone, then into the colour targets. Each draw has modules
and a pipeline of its own, and the depth-only one, the smaller, is built first. Drawn without its colour draw it leaves
the list's depth in the scene with nothing drawn over it: what lies behind fails the depth test there, and the sky's
colour shows through the ground in the shape of the grass for as long as the colour draw's pipeline builds, seconds each
on the drivers the fallback is the default for. So while a list's colour draw is skipped for its modules or pipeline,
its depth-only draw is skipped too, and the ground shows as it does without the grass. A list is its record's address in
the game's memory. The colour draw stamps the list with the present it was skipped in and clears the stamp when it is
drawn; the depth-only draw, which comes first in the frame, is held when the stamp is at most `MFB_HOLD_PRESENTS`
presents old, so a list whose colour draw no longer comes is held no longer than that. A draw whose stages write memory
is never held (`mfb_draw`): the writes are wanted whatever is drawn. The table is `runtime/gfx/mesh_fb_hold.h`'s
(`tests/mesh_fb_hold_check.c`), read and written by the replay's threads without a lock: a lost update costs a list one
frame of the hold. `NX2SARIA_MESH_FALLBACK_HOLD=0` turns it off.

### `mfb_hold_set()`

The list's colour draw was skipped for its modules or pipeline (waits 1), or was drawn (0).

### `mfb_hold_skip()`

A held depth-only draw is left out, counted as a draw whose own pipeline is building: the frame lacks it for the same
reason (`draw_skipped`, `runtime/vk/vk_backend.c` `frame_hold()`).

- `if (!(n & (n - 1)))`: the first, then at powers of two.

### `mfb_hold_colour()`

A colour target is bound (`OP_SET_RT`), whatever the draw's write masks are.

### `mfb_hold_role()`

What a fallback draw with state s writes: 1 a colour target, 2 depth and no colour, 0 neither.

### `prepare_draw_ex()`

- `int fb = mesh == 2`: from here on mesh says what the draw is, and `!fb` that its pipeline is a mesh pipeline.
- `if (ex->pass_noatt)`: no attachment: the pass covers this draw's viewport or scissor, reopened when it needs more.
- A new pass may have another scale (`pass_scale`): the viewport, the scissor and the slope bias (`slope_bias`) follow
  it.
- `if (n_skip_prog && skip_prog_wanted(ex->prog->serial)) return draw_skipped(ex, mesh, SK_PROG_FILTER)`: -1 until read.
- `uint32_t gen = memo ? vk_texture_slots_gen() : 0`: before `shader_for`, whose lookups may bump it.
- A draw into a target the previous frame did not render (or into none) waits for a pending shader or pipeline: one-shot
  draws (volumes and LUTs generated once, later only sampled) would leave their target empty for the session. A draw
  into per-frame targets is skipped until they are ready and comes back next frame, unless its shaders write memory:
  then it waits like a one-shot draw (`draw_writes`).
- `int oneshot = -1, skip = pipe_skip_mode()`: -1: not looked up.
- `int writer = 0`: the draw waits as one-shot by `draw_writes` alone.
- `vk_exec_ahead(ex)`: the later draws' shaders and stage libraries are queued while this one waits or is skipped.
- Only the stages that are here can say that they write: one still being translated has no reflection yet.
- `if (!oneshot)`: skipped until ready.
- From the frame's wait pool (`NX2SARIA_ONESHOT_BUDGET=0`: until each is translated); a shader the pool does not reach
  stays at the front of the translators' queue, and the draw is skipped like a per-frame one.
- `if (stutter_map_on)`: the shaders translated by now name it; one still translating is not known yet.
- `if (vk_dlaa_skip_draw(ex, fs)) return draw_skipped(ex, mesh, SK_DLAA)`: The game's FXAA off, or its scaling replaced
  by DLSS (`runtime/vk/vk_dlaa.c`).
- `if (fb) memset(s.vattr, 0, sizeof s.vattr), memset(s.vstream, 0, sizeof s.vstream)`: whatever the last vertex draw
  left.
- `if (result) requested = s`: the mesh caller checks the state that selected its cache entry.
- A draw of at most 6 vertices over half the render area or more (a full-screen triangle or quad, a tile) waits for its
  pipeline: skipping it can leave a black tile. Those waits and the one-shot draws' come from the frame's wait pool
  (`frame_wait`; `NX2SARIA_PIPE_SKIP=fullscreen` skips the full-screen draws instead, and with
  `NX2SARIA_ONESHOT_BUDGET=0` a one-shot draw waits up to `ONESHOT_WAIT_MS` outside the pool). Everything else waits for
  its fast link from the small budget of `link_budget` and is skipped while its pipeline is built.
- `uint64_t *budget = NULL`: the per-frame budget a timed wait is charged to.
- `int linking = 0`: the wait is the short one for a fast link (`link_budget`).
- `int fullscreen = 0`: the wait is a full-screen draw's.
- A frame that has left a draw out already is not shown, or is shown without that draw (`pending_draws`,
  `runtime/vk/vk_backend.c` `frame_hold()`): a wait for another pipeline would only make it later.
- The previous draw's pipeline while its state repeats (PM, `NX2SARIA_PIPE_MEMO=0` turns it off): the cache key is made
  from the state alone, and a pipeline once found only changes from its fast link to its optimised build, read from the
  info the cache entry keeps.
- `uint64_t w0 = 0, m0 = 0`: m0: when the lookup missed (`wait_note`).
- Nearly every draw finds its pipeline, so the first lookup waits for nothing and the clock is read only for a draw that
  missed. A miss first has the rest of the submission read ahead (`vk_exec_ahead`: the stage libraries of the later
  draws' pipelines build on the workers from here on), inside this draw's wait budget.
- The fast link's wait is one for each pipeline, counted from its first miss (`link_budget`): what that left unbuilt is
  a compile, and its later draws, in this frame and the next ones, wait for nothing.
- A mesh pipeline without stage libraries that is known to be compiling gets no fast link, so the few milliseconds a
  draw waits for one are lost in every frame that meets it until the compile ends, 0.2 to 1.1 s later (`MESH_WAIT_MS`).
- Still pending after the wait any draw gets: a draw into targets that are not per-frame waits as one-shot, and so does
  a draw into per-frame targets whose shaders write memory (`draw_writes`).
- A full-screen draw still pending after its budget: skipped, it leaves the frame black or unlit. While the previous
  picture can stay on screen in its place (`vk_frame_hold_left`) that is what happens; once that time is used up the
  draw waits on, from the `FULLSCREEN_LAST_MS` of its frame, before such a frame is shown.
- Also a one-shot draw skipped at the end of its wait, however short: its target may not be drawn again.
- `if (fb && cx->mfb_list)`: the list's depth-only draw goes with its colour draw (`mfb_hold_role`).
- `if (pending) return draw_skipped(ex, mesh, SK_PIPE_PENDING)`: a background worker is building it: skipped until it is
  ready.
- Only the dynamic states whose values changed (`vk_pipeline_set_dynamic_delta`); `NX2SARIA_DYN_DELTA=0` records them
  all after every pipeline bind, as before.
- An emulated mesh draw (`NX2SARIA_MESH_EMULATION`) binds a vertex pipeline: its topology, primitive restart and vertex
  input are dynamic and are set as for any vertex draw.
- `if (vk_draw_marks_on)`: `NX2SARIA_DRAW_CRUMBS`: a lost device then names these shaders
  (`runtime/vk/vk_crumbs.c` `vk_draw_mark()`).
- `vk_pipeline_line_width(ex->cb, &s, ex->line_width)`: a draw of lines: the game's width; else nothing.
- `vk_dlaa_jitter_check(ex)`: a draw with another jitter than the viewport recorded last records it again.
- The replay's binding path is the CPU cost the frame profile needs to separate from recording.
- `if (vk_texture_deferred()) return 0`: a chunk's lookup waits for its turn: the op is replayed again.

### Looking ahead

(`runtime/vk/vk_exec.c` `vk_exec_ahead()`): the binds and draws of ops the replay has not reached, applied to sx, a copy
of the replay state. A draw looks its shaders up as `prepare_draw_ex` will (one not in memory is queued for its disk
read or translation by that lookup) and names its state to `vk_pipeline_hint`. The last draw looked up is kept: one with
the same program, texture binds and state repeats it, as most draws of a pass do. A mesh draw is looked up with its mesh
and task stages where its pipeline will be a mesh pipeline with stage libraries (`vk_pipeline_mesh_libs`; never with the
mesh fallback, which draws through vertex pipelines of its own).

### `ahead_draw()`

- `if (!has_vs) return`: a program without the stage this draw starts from: no libraries.

### Per-object motion

The camera pass (`runtime/vk/vk_dlaa.c` `mv_pass()`) gives everything the camera's motion, so a character that moves
with the camera, or on its own, gets the wrong one and DLSS throws its history away there. Each solid scene draw whose
position reads an object's own uniform blocks (bones c5, place c7, c6; `mvobj_draw_` says which are left out and why) is
therefore drawn a second time (listed during the replay, MQ, and recorded by `mv_pass`) with its vertex shader's
last-frame variant (translator patch 0015: the position again from last frame's blocks, bound at slot bank + 15, and
last frame's camera, written to a spare output) and the fragment shader of `mvobj_fs_spv.h`, which writes last frame's
window position minus this frame's. The draw keeps everything else of the original: vertex and index buffers, the other
uniform blocks, the jittered viewport and the scene depth, tested with the game's own test and its equal case (LEQUAL,
GEQUAL) without writes, so only the visible surface writes its vector. Last frame's blocks: `vk_mvprev_find` and
`mo_prev_block`; the camera: `vk_dlaa_prev_camera`. A draw whose variant or pipeline is still building, or whose
last-frame blocks are not in the snapshot, keeps the camera's motion. `NX2SARIA_DLAA_OBJMV=0` turns it off
(`runtime/vk/vk_mvprev.c`).

- `MO`:
  - `const GfxShader *fs[16], *link_fs[16]`: per input Location.
  - `uint64_t ns_part[3], n_part[3], misses, cyc[3], ncyc, big`: checks: filter, data and stillness (memo misses),
    variant and pipeline; time spent: checks and lookups of every candidate; recording the draws again; cand: draws
    whose vertex shader reads c5 or c7.
  - `uint64_t why[5]`: nodata by cause: camera, bones, place, bones copy, place copy.
  - `uint64_t no_out, storage, no_fs`: variant without the extra output, with storage resources; no fs.
  - `uint64_t other_cam`: listed draws left out: their camera block is not the scene camera.
  - `uint64_t lk[14][5]`: last-frame lookups per binding index: none, self, side, nearer, refused (another object).
  - `uint64_t e_draws, e_pipe, e_dyn, e_push, e_pushw, e_sets, e_vb, e_setw, e_setreuse`: emitted commands (summary).
- `MoDraw`: The motion draws of the frame, kept as a list while the scene replays and recorded into the replay's own
  command buffer by `vk_mvobj_execute`, through the recorder thread (`runtime/vk/vk_rec.h`) like every other draw:
  recording them straight into a secondary command buffer made the driver's work for each one on the replay thread. What
  a draw shares with the one before it (pipeline, dynamic state, viewport, buffers, descriptors) is recorded once; a
  push descriptor set is pushed again only where a binding changed (pushes update the set in place while its layout
  stays bound).
  - `uint32_t st, desc, vb`: indices into MQ.st, MQ.desc (the draw's packed descriptors) and MQ.vb.
  - `VkDescriptorSet set[2]`: allocated sets; `VK_NULL_HANDLE` for a pushed one.
  - `uint64_t cam`: the draw's camera block, this frame's.
  - `int skin`: `runtime/vk/vk_skin_motion.inc`: 0 not a skinned character's draw; 1 one, 2 one checked against last
    frame's depth; -1 one `skin_resolve` left to the camera.
- `MQ`:
  - `uint64_t frame, fence`: the frame and submission the list belongs to.
  - `NVNbufferRange vin[17]`: the vertex ranges `MQ.vb[nvb - 1]` was resolved from, under `vk_mem_gen` vgen.
  - `float sup_in[3]`: the support buffer last filled for the list and its inputs.
  - `float par_in[8]`: the fragment shader's parameters last written and their inputs.
  - `VkDescriptorSetLayout aset_layout[2]`: the last allocated set per set number, with its descriptors.
- `MM`: The position bank masks of the draws considered (`gfx_shader_mvprev_banks`), with counts, for the summary.

  ubo, ssbo, tex: the first vertex shader's.
- `mo_gen`: the list's snapshot (`mo_prev_cached`), bumped by `mo_begin`.
- `MF`: this list's draws per position banks and changed banks.
- `fm_last`: the last executed list's, formatted (`vk_mvobj_frame_masks`).

### `mo_begin()`

Starts this frame's list (the old one, never executed, is dropped).

### `link_motion_fs()`

- `OpDecorate` Location 15 uniquely identifies the previous clip position in `runtime/shaders/link_motion.frag`.

### `mo_write()`

The write of binding b from its packed descriptors.

- `w.pBufferInfo = &d->buffer`: `VkDescSlot` is 24 bytes, as `VkDescriptorBufferInfo`: arrays line up.

### `mo_desc()`

The descriptors of a motion draw, packed into MQ.desc at `q->desc` without the binding caches of the command buffer
being replayed (B, DS, RS), which then stay true for its next draw: uniform buffers (through `ubo_info`, so with
`mo_ovr`), the support buffer and sampled textures. An allocated set is written here (or the last one reused when it
would hold the same); a pushed set is pushed by `mo_emit`. 0 for a pipeline with storage buffers, images or texel
buffers (not drawn again).

- `memset(d, 0, total * sizeof *d)`: unused bytes zero: equal descriptors compare equal.
- `} else if (b->kind == GFX_RES_SSBO && !(r->flags & 1))`: read only: instance data of foliage and such.

### `mo_vertex_buffers()`

The draw's vertex buffers as `bind_vertex_buffers` resolves them, into MQ.vb; the previous entry again while the ranges
and `vk_mem_gen` are the same (most motion draws in a row are one model's parts).

### `mo_emit()`

Records the list into cb (the replay's command buffer, inside the motion pass's rendering), skipping what the draw
before already set.

- `int pdesc[2] = {-1, -1}`: the pushed set's descriptors (MQ.desc index) under lay/pinfo, -1 none.
- The bindings that differ from the last push of this layout and pipeline (all after another).

### `mvobj_draw()`

The chunks of a parallel group (`runtime/vk/vk_exec.c`) add to the frame's list at once: whether a draw is listed at all
is decided by its own draw context (`mvobj_pick`, its memo per context), and only the frame's list, its memos and
statistics go under `mo_lock` there: under one lock for the whole check, the chunks' scene draws waited for each other.
The order of the list does not change the picture: its draws write motion without blending, tested against the scene
depth they do not write.

- The upscaler, or frame generation without one (`vk_dlaa_fg_only`), takes the motion.
- `if (timed < 0) timed = rt_knob("MVOBJ_TIMES", "0")`: `NX2SARIA_MVOBJ_TIMES=1`: the summary's times (the clock reads
  cost).

### `mvobj_banks()`

`NX2SARIA_MVOBJ_BANKS=1` (test): for scene draws, the words of each other uniform bank (c3, c6, c8-c16) that changed
since the same shader's draw last frame, logged at frame 3000: where the time that moves wind and water lives.

### `mo_similar()`

1: old is cur a little changed or the same (every float within 10 %, at least 1.0 and at most 4.0); `*changed`: not the
same. The cap: the game's world positions are in the thousands, and 10 % of them let another object's block a few units
away pass as this one moved; nothing moves 4 units in a frame.

- Word by word as integers; float arithmetic only on two normal floats (the blocks hold integers too, and denormal
  arithmetic took this loop 1.9 million cycles a block).
- `if (!ex || ex == 0xff || !ey || ey == 0xff) continue`: not both normal floats: an integer or a tiny value.

### `mo_prev_block()`

Last frame's copy of a block, accepted when it is this frame's block a little changed (`mo_similar`, which see): a block
of another object the address rule found would make motion up. 1 and `*out`: found (the block itself when it is
unchanged, `*changed` 0); 0: none, the draw keeps this frame's block there.

- c9 and c11 keep this frame's block. With the copies the address rule finds for c11 and last frame's c9, the c4 c7 c9
  c11 draws (walls, cliffs, arches) got several pixels of motion under a still camera (`NX2SARIA_MV_CHECK`, those draws
  alone: 34 % of the picture moving; with c11 kept, 0.8 %). With c11 kept, their c9 still changes every frame while
  nothing moves (the check's per-draw line: about 120 such draws a frame changed in c9 alone), the object scale the
  picture shows is 0.6-0.8 of theirs, and with Link stopped against a cliff 40-77 % of the picture got 1-3 px of motion
  that flipped direction, three times the warp error of the camera's vectors. With c9 kept too, the turning run is as
  good (object warp error 0.46 of the camera's). Why their copies are wrong is not known.
  `NX2SARIA_MVPREV_SKIP=<hex bank mask>` (test) replaces the default a00 (bit n: cn).
- `if (how == MVPREV_SELF) return 1`: bound at the same address last frame: not written again.
- `static long trust = -2`: `NX2SARIA_MVPREV_TRUST=<hex bank mask>` (test): the copy found is taken without the check.

### `mo_prev_cached()`

`mo_prev_block` once per block and list: many draws share their blocks (the scene's c14, a model's c7 and c9 across its
parts), and the lookup with its comparison was most of the checks' time.

### `mo_legacy_previous()`

- `have |= 1u << b, moving |= changed << b`: the banks whose block changed.

### `mvobj_pick()`

Whether the draw goes on to the frame's list (`mvobj_draw_`), with its motion variant and the banks its position reads;
this draw context alone decides (no lock in a chunk): the draw's state, and the variant memo per context.

- `if (!vk_exec_in_chunk()) mvobj_banks(ex)`: a test's log of every scene draw (its tables are not shared).
- Solid geometry only, which tests and writes the scene depth: effects, particles and screen quads write none, and drawn
  again they would put their motion on the surfaces behind them (DLSS takes those from the surface).
- An opaque colour pass can use EQUAL against a depth prepass. Its geometry is already in the scene depth.
- The player's own draw (ownership records), else any other skinned character's (`runtime/vk/vk_skin_motion.inc`).
- A vertex shader that reads a user bank besides the camera's (c4) may move on its own.
- The variant: the translation of the same code and answers, kept per shader once found; it names the banks the position
  depends on (0: none, the shader is skipped from then on).
- `if (!RT_V111) vk_mvprev_want((banks & ~(1u << 4)) >> 3)`: binding index = bank - 3.
- A position without the scene camera (c4) has a view of its own (c8 c9 c10 `[c13]`: its c8 is at a new address every
  frame, last frame's is never found): drawn again it would get this frame's view and no camera motion
  (`NX2SARIA_MV_CHECK` while turning: 6.2 px end-point error against the camera vectors' 1.7).
- `if (noc4 < 0) noc4 = rt_knob("MVOBJ_NOC4", "0")`: `NX2SARIA_MVOBJ_NOC4=1` (test): drawn again all the same.
- Only positions that read an object's own blocks: bones (c5), place (c7) or c6. The rest (terrain and grass: c14 c15
  c16, with or without c9) measured no better than the camera's motion (`NX2SARIA_MV_CHECK`, turning, those draws alone:
  object warp error 0.112 against the camera's 0.113, the object part 0.4 of what the picture shows) and were half the
  draws checked and listed. `NX2SARIA_MVOBJ_ALL=1` (test) takes them again.
- `NX2SARIA_MVOBJ_ONLY=<hex bank mask>` (test): only draws whose position reads no bank outside the mask (bit n: cn).
- `static long exact = -2`: `NX2SARIA_MVOBJ_EXACT=<hex bank mask>` (test): only draws with exactly these position banks.

### `mvobj_draw_()`

A draw `mvobj_pick` took, onto the frame's list (in a chunk under `mo_lock`): the list begins at the first such draw of
the frame's submission (`mo_emit` drops a list of another frame, submission or depth format).

- Last frame's camera and blocks; held still when none of the position's blocks changed (most of the scenery).
- Missing visibility evidence is not evidence that this valid pose vector is wrong. Keep the raw vector, with the normal
  current-depth and history-validity checks.
- Another skinned character's draw: its blocks, and whether a bone turned sharply (`runtime/vk/vk_skin_motion.inc`);
  which draw of last frame it continues is decided when the vectors are made (`skin_resolve`).
- `pose_visibility_guard = 0`: as for the player: no depth of last frame to check against, the plain vector.
- Blending off (`blend_en` below); the factors stay the game's, valid NVN values.
- `ms.chmask.mask = 0x3`: r and g of target 0.
- The game's own test with its equal case (LESS -> LEQUAL, GREATER -> GEQUAL): the surface the draw left in the scene
  depth passes, anything nearer hides it (`NX2SARIA_MVOBJ_EQUAL=1`: EQUAL, for comparison).
- The pipeline per state, kept: the lookup hashes and locks the whole state.
- `static int plast = -1`: the entry of the last draw: a model's parts in a row share the state, no hash then.
- `pipe = __atomic_load_n(&info->pipeline, __ATOMIC_ACQUIRE)`: its optimised build once swapped in.
- `if (t0) MO.ns_check -= t1 - t0`: counted as recording below.
- Into the frame's list, past the primary's binding caches (`mo_desc`); `mo_emit` records it.
- Where `skin_resolve` puts last frame's bytes of each block, once it knows whose they are.
- `for (int i = 0; i < 16; i++)`: the frame's draws per position banks and changed banks (`vk_mvobj_frame_masks`).

### `vk_mvobj_execute()`

- `if (exec < 0) exec = rt_knob("MVOBJ_EXEC", "1")`: `NX2SARIA_MVOBJ_EXEC=0`: listed, never recorded (test).
- Only draws with the scene camera block (`runtime/vk/vk_dlaa.c` `mv_pass()`'s): last frame's camera they get is that
  block's, and a draw of another view would move by the difference between the two views.
- `(!MQ.d[i].skin || (MQ.d[i].skin > 0 && SK.resolved)))`: a skinned draw: one `skin_resolve` paired.
- `else if (!MQ.d[i].skin) MO.other_cam++`: a skinned draw left out has its own count (`skin_status`).
- `vkCmdBeginRendering(cb, &ri)`: through the recorder (`runtime/vk/vk_rec.h`), in order with the draws and the end.
- The draws left their pipeline, descriptors, buffers and dynamic state bound: the next game draw sets everything.

### `draw_mesh_fn()`

NVN2 mesh draws (DrawMeshTasks): count task workgroups, or mesh workgroups for a program without a task stage, through
`VK_EXT_mesh_shader`. The stages are the translator's TaskEXT / MeshEXT modules (`tools/xlate/MeshSpirv.cs`). The lazy
lookups below store the pointer before the flag that says it was looked up: the chunks of a parallel group draw meshes
at once, and one that saw the flag first went on with no pointer and skipped its draws.

### `mesh_shading_rate()`

`NX2SARIA_FOLIAGE_SHADOW_VRS` (0 to 3, default 2): the foliage's mesh draws in the shadow cascades from this one on
shade one fragment per 2x2 pixels; 3 turns it off. A leaf's alpha test then runs a quarter as often, and the far
cascade's texels are metres across the forest floor, where the coarser cut-out does not show. A mesh pipeline has the
rate as dynamic state, so every mesh draw sets it.

- `if (!__atomic_load_n(&looked, __ATOMIC_ACQUIRE))`: as `draw_mesh_fn`: the pointer before the flag.

### Mesh fallback

A mesh draw on a device without `VK_EXT_mesh_shader` (`mesh_fallback`, `runtime/vk/vk_device.c`), or on any device with
`mesh_fallback` = 1. The program's task and mesh stages run as compute dispatches that write the fallback's buffers, and
an ordinary draw reads them (`runtime/gfx/mesh_fb.c` makes the modules, `runtime/vk/vk_mesh_fb.c` holds the buffers):

- The draw's record: its workgroup count, copied on the GPU when the game's own record holds it.
- With a task stage only, the task dispatch: the task module, with the task stage's bindings.
- With a task stage only, the scan: lists the mesh workgroups the tasks started and counts them into the record.
- The mesh dispatch: the mesh module, with the mesh stage's bindings.
- The draw: one instance per mesh workgroup, with the generated vertex stage and the program's fragment stage, the
  instance count from the record.

Every step reads what the one before wrote, so a full barrier stands between them, and the dispatches cannot be recorded
inside a pass: each such draw ends the open pass, and the next draw begins it again. The buffers are reused by every
draw (`runtime/vk/vk_mesh_fb.c`), which the barrier before a draw's first dispatch makes safe: the previous draw's
vertex stage has read them by then.

Batched (`NX2SARIA_MESH_FALLBACK_BATCH`, `mfb_batch_on`, `runtime/gfx/mfb_batch.h`), a pass's draws record their steps
in the part's pre-pass command buffer instead, each draw into a part of the buffers of its own, and in the pass only the
draw: the pass stays open. The first batched draw of a pass cuts the part there, so what came before runs before the
steps.

The modules of a program are made once, on a thread of their own, with their compute pipelines (a mesh program of the
game is up to 900,000 SPIR-V words, and its pipeline compiles for as long as the mesh pipeline it replaces): until they
are ready the program's draws are skipped, like draws whose pipeline is being built. Without background pipeline builds
(the probes) they are made at the draw.

- `MfbCs`:
  - `const GfxShader *ms, *ts`: the key: the translated mesh and task shaders (ts NULL: none).
  - `volatile int32_t state`: 0 queued or being made, 1 ready, -1 failed.
  - `const GfxShader *cs_mesh, *cs_task`: the compute modules (`gfx_shader_own`).
  - `MfbHist hist`: what its draws asked for (`runtime/gfx/mfb_batch.h`; `runtime/vk/vk_mesh_fb.c` `record_check()`).
  - `struct MfbCs *next`: the worker's queue.
- `MFB`:
  - `int threads, busy`: workers started, workers making a program now.
  - `uint64_t first_ms, drained`: the first program's queueing; the queue has emptied once (logged).

### `mfb_binding_base()`

One past the highest binding the module declares.

### `mfb_module()`

The compute module of stage (6 task, 7 mesh) of e, as a shader of the runtime's own with the stage's resources and the
fallback's buffers.

### `mfb_worker_count()`

The programs queued at their first draws are made by `NX2SARIA_MESH_FALLBACK_WORKERS` threads (auto: one for each 4
logical processors, 1 to 4). A place's mesh programs all arrive in its first frames and each one's draws are left out
until its compute pipelines are built (up to seconds each on some drivers), so with one thread the foliage of a first
visit filled in one program at a time. The first time the queue empties the log says how long it took.

### `mfb_worker()`

- `plat_thread_priority(PLAT_PRIO_BELOW_NORMAL)`: as the pipeline workers' optimised builds: the game's threads go
  first.

### `mfb_cs_get()`

The entry of (ms, ts), made and queued at its first draw; NULL (logged) when the program's layout is not carried.

- `mfb_cs_tab[i] = e`: a program that is refused is not looked at again.

### `mfb_vs_get()`

The vertex stage that draws e's mesh buffer slots for fragment shader fs: an output for every input fs reads, in its
type.

### `mfb_dispatch()`

One of the draw's compute dispatches: module cs with the bindings of NVN stage `stage`, its group counts read from the
record at `at`. 0 when its pipeline is not there. mark: the dispatch's pass mark (`dispatch_mark`); not in the pre-pass
command buffer, whose commands run before the part's marks around them.

### `mfb_step()`

A step of the stages: one by one, `vk_exec_barrier` (the first ended the pass); batched, `ex->cb` is the pre-pass
command buffer, which has no pass.

### `mfb_stages()`

The draw's stages in `ex->cb`, from its record to its note: the record's copy, the clamp of a count the GPU wrote, the
task dispatch and the scan, the mesh dispatch. The draw after reads the record at `*at` in `*rec`. 0: a dispatch's
pipeline failed.

- `if (ind)`: to what the buffers hold, before a dispatch reads it (`runtime/shaders/meshfb_clamp.comp`).
- `cx->B.pipe[1] = VK_NULL_HANDLE`: the clamp bound its own.
- `cx->B.pipe[1] = VK_NULL_HANDLE`: the scan bound its own.
- The note copies the record's report words, which the steps before wrote (the mesh dispatch only reads the record):
  batched, no barrier between the mesh dispatch and the note, so a batch's draws are one barrier apart fewer; the next
  draw's parts and record do not overlap this one's (`mfb_part_take`, `vk_meshfb_record`).
- `if (!batched) vk_exec_barrier(ex)`: batched, the barrier at the pre-pass command buffer's end (`vk_meshfb_pre_end`).

### `vk_exec_mfb_batch_exit_log()`

- `MFBB`: `NX2SARIA_MESH_FALLBACK_BATCH` (`auto|0|1`, `mfb_batch_on`): the stages of a pass's fallback draws run in the
  part's pre-pass command buffer (`vk_sync_pre`), as the mesh emulation's do (`NX2SARIA_MESH_EMU_BATCH`), and the pass
  is not broken for them. One by one, each draw ended the pass, ran its stages with a full barrier between every two and
  began the pass again: about 33 us of the submitting thread a draw ([replay-cpu.md](../replay-cpu.md)), 1.7 ms a frame
  in Kokiri Forest and 3.2 ms in Hyrule Field. Which draws are batched and the size of their parts of the buffers:
  `runtime/gfx/mfb_batch.h`. 0: every draw one by one. Each pass with batched draws cuts the part (a new command buffer
  and pre-pass command buffer, the state bound again), and a batched draw keeps its barriers and bindings in the
  pre-pass command buffer: on a CPU-bound PC that costs about what the pass breaks saved (`mfb_batch_on` has the bench's
  numbers); the gain is the GPU's, without the pass breaks.
  - `MfbCount n`: the draws, batched or one by one and why, and the cuts (`runtime/gfx/mfb_batch.h` `mfb_count()`).

`runtime/gfx/gfx_present.c` `rt_exit_steps()` (`plat_exit` runs no atexit handlers): the session's totals once more,
with batching on.

### `vk_exec_mfb_batch_counts()`

`runtime/gfx/mesh_fb_probe.c`: `why[MFB_REASONS]`, `cuts[3]`.

### `mfb_batch_on()`

The switch, read once: at the device's start with the mesh fallback (`runtime/vk/vk_device.c`,
`vk_exec_mfb_batch_start`), so the log says its value even in a session that draws nothing through the fallback.
`NX2SARIA_MESH_FALLBACK_BATCH`: 1 (the default), 0 (off, false, no: every draw one by one), or auto (on where the
fallback is the device's default: `runtime/vk/vk_mesh_path.h` `mesh_path_fallback_default()`, AMD's Windows driver for
RDNA 1 and 2, Intel, a device without mesh shaders; off where `mesh_fallback` = 1 forces it on a device that draws them
natively). On by default: on the Linux bench (RX 6700, RADV, the CPU sets the frame rate, the fallback forced), two A/Bs
of six interleaved pairs each gave Hyrule Field +1.9 and +3.3 fps, Kokiri Forest +0.3 and +1.6 (medians of the pairs'
differences), the GPU's time per frame 0.4 to 1.4 ms less (2026-11-22), and the pictures as main draws them. The
device's path and default come from `vk_exec_mfb_batch_start` (`runtime/vk/vk_device.c`, before `vk_ctx()` is ready); a
first call from a draw without them (the probe, which turns the fallback on itself) asks `vk_ctx()`. The words are read
as the settings file reads a switch's: off, false, no and 0 are 0.

### `mfb_batch_log()`

Every 30 s while the fallback draws with batching on, and at exit: the draws batched and those one by one, by reason
(the way back). Nothing with the switch at 0: every draw is one by one then, as the start line says.

### `mfb_step_cut()`

`mfb_batch_steps`' calls in `mfb_draw`: the cut, the pre-pass command buffer, the part of tb, lb and mb bytes.

### `mfb_draw()`

A mesh draw of `count` workgroups, or of the count in the game's record at ind (4 bytes, GPU-written), which lies at
`list` in the game's memory (0: no record).

- The list's hold (`mfb_hold_role`): not for a draw whose stages write memory.
- Without a colour target the draw is its list's depth-only one, known before its dispatches are recorded; with colour
  targets bound and their write masks off it is known where its state is (`prepare_draw_ex`).
- A target the game does not render every frame: the draw may not come again, so it waits for its modules from the
  frame's wait pool, as such a draw waits for a pipeline (`prepare_draw_ex`).
- Its modules are being made: with a colour target bound it counts as the list's colour draw. Its write masks are not
  known here; a depth-only draw taken for one holds itself for `MFB_HOLD_PRESENTS` presents at most.
- `if (count > most) count = most`: a count the GPU wrote is cut on the GPU, below.
- `vk_meshfb_poll(vk_sync_fence_done())`: the histories, from the records of finished submissions.
- The steps (`runtime/gfx/mfb_batch.h` `mfb_batch_steps()`): a task or mesh stage that reads a texture or image goes one
  by one (its binds in the pre-pass command buffer may record an image's first fill or an alias copy,
  `vk_exec_inline_begin`, which ends and begins the part's pass around it). The first batched draw of a pass cuts the
  part there (as the mesh emulation does, `mesh_emu_draw`): what the part held before, the game's culling and the draws
  done one by one among them, runs before the pre-pass command buffer, which begins with a full barrier; again when that
  pass ended since (`mfb_need_cut`), after a draw one by one wrote over the parts, and when the part does not fit what
  is left.
- A part larger than the whole buffers never fits: one by one without a cut (a cut for it is for nothing).
- `if (why != MFB_BATCHED) mfb_count(&MFBB.n, why, cut, 0)`: a batched draw counts once its stages are in.
- `vk_meshfb_one_by_one()`: after a batch's parts were taken, it writes over them: a cut before the next batched.
- `vk_exec_barrier(ex)`: ends the pass; the culling's record and the last draw's reads come before what follows.
- The stages in the pre-pass command buffer, each after the last with a full barrier of its own (`vk_exec_barrier` is
  the part's, with its pass); the replay's compute pipeline there is not the part's, and its commands are not the part's
  (`ex->recorded`: whether a barrier of the part has anything to order).
- `cx->B.res_valid = 0`: the vertex stage reads its own part of the mesh buffer: no bind skip into or out of it.
- `if (cut && ex->in_pass) ex->pre_pass = ex->pass_serial`: the pass the prepare began: its batched draws share pre.
- `if (ex->mark && ind) ex->mark->mesh_ind++`: the pass's mesh draws (`runtime/vk/vk_crumbs.c` `describe_mark()`).

### `mesh_emu_draw_prim()`

The primitive an emulated draw's mesh stage outputs: point meshes draw point lists, the rest triangles.

- `const uint32_t *w = gfx_shader_spirv_pin(ms, &n)`: once per mesh shader the memo meets.

### `mesh_emu_on()`

- `on = vk_ctx()->mesh_emulation`: decided with the mesh fallback at device creation (`runtime/vk/vk_mesh_path.h`).

### `op_draw_mesh()`

- `if (on < 0) on = rt_knob("MESH", "1")`: `NX2SARIA_MESH=0`: mesh draws are skipped (diagnosis).
- `if (ex->mark) ex->mark->mesh++`: the pass's mesh draws, for a lost device's report
  (`runtime/vk/vk_crumbs.c` `describe_mark()`).

### `mesh_ring_release()`

- `enum { MESH_RING, ... }`: The record NVN's DrawMeshTasksIndirect reads is {count, first} (as `NV_mesh_shader`'s), and
  Vulkan's {x, y, z}: read as it is, y = first (0) drew nothing. Each draw gets an entry of this ring, {count, 1, 1}: y
  and z filled with 1 and x copied from the game's record on the GPU, between the pass's end and the draw (the record is
  written by the culling compute earlier in the frame and never comes back to guest memory). `first` is not applied: the
  game leaves it 0. `MESH_RING` entries outlast the frames in flight: an entry is rewritten only after a barrier behind
  its last draw.
- `vk_gpu_passes_capturing()`: `NX2SARIA_GPU_PASSES`: the workgroup counts of the indirect mesh draws of the timed
  frames, copied to host memory.

`vk_exec_draw_shutdown`: a probe ends with the device idle.

### `mesh_ring_make()`

- `enum { MESH_AHEAD, ... }`: `NX2SARIA_MESH_AHEAD` (default 1; 0 = each draw by itself, "ab" switches with the A/B
  blocks): the copy of a record cannot be recorded inside a pass, so every indirect mesh draw ended its pass, and a pass
  that draws its meshes one after another was cut into as many pieces (a village: 45 of them a frame, each with a gap of
  its own on the GPU). The first such draw of a run now fills the entries of the ones that follow it as well, with
  nothing but state, draws and counter reports between them
  (`runtime/vk/vk_exec.c` `op_linear()`, `vk_exec_mesh_run()`): those draws then find their entry and stay in the pass.
  The culling compute that writes the records cannot run inside that run, and an entry is taken only by the draw it was
  filled for, at the same record address, while no other op was replayed since (`vk_exec_brk`). In the village 42 of the
  45 draws a frame are served that way: 3 gaps with these copies a frame where there were 41 (0.04 ms of GPU time for
  0.22), and about 40 passes fewer to begin and end. The entries filled ahead are the draw context's (`DrawCtx.ma`): a
  chunk of a parallel group (`runtime/vk/vk_exec.c`) fills them for the draws that follow its own (`vk_exec_mesh_run`
  reads on from the op it records), and an entry it filled for a draw another chunk records is left unused.
- `mesh_ahead_taken`: since the start: draws served by an entry filled ahead; the others.

The ring, made before the chunks of a group record (`vk_exec_draw_group_begin`) or at the first indirect mesh draw.

### `mesh_ring_entry()`

A ring entry {count from src, 1, 1}, filled in cb; `ex->cb`: after a full barrier, which ends the pass (the next draw
opens it again) and orders the culling's writes before the copy. Another cb (the pre-pass one of the mesh emulation)
begins with one. d: the draw it is for, which may take an entry filled ahead (`MESH_AHEAD`, `ex->cb` only); NULL: none
(the mesh emulation's).

- `cx->ma_n = cx->ma_i = 0`: the walk went another way: the entries left are never drawn from.
- `if (!vk_mem_resolve(f->ind, 12, &ip) || ip.offset % 4) break`: that draw reports it when the walk gets there.

### `foliage_skip()`

`NX2SARIA_FOLIAGE` (full, reduced, off; default full; foliage in nx2navi.ini): Ocarina draws its foliage with
DrawMeshTasksIndirect, the instances its compute passes culled on the GPU. With a task stage: the small grass and moss
that carpet the ground; without one: ferns, leaves, clover and the other plants. Its other mesh draws (DrawMeshTasks
with a count: terrain, rocks, buildings, doors) are not foliage. reduced skips the indirect draws with a task stage, off
every indirect one, in every pass (their shadows too). In a frame dump of Kokiri Forest on an RX 580 (f8118, 2026-11-10)
the grass and moss were one draw with task stage 2ddf7b0bf74e5a05; the plants nine without.

### `op_draw_mesh_ind()`

DrawMeshTasksIndirect: one draw from its ring entry (`mesh_ring_entry`); x2-x4 of the call vary like leftovers, so they
are not read as a count or stride.

- `if (emu)`: emulated: a ring entry {count, 1, 1}, made where the stages run, is the first stage's dispatch.
- `if (ex->mark) ex->mark->mesh_ind++`: the pass's mesh draws, for a lost device's report
  (`runtime/vk/vk_crumbs.c` `describe_mark()`).

### `NX2SARIA_MESH_EMULATION`: mesh draws on GPUs without `VK_EXT_mesh_shader`

Each mesh draw (prepared like one, `prepare_draw_ex`, for its layout, descriptors and graphics state) runs its task and
mesh stages as compute pipelines (`vk_pipeline_mesh_emu`) outside the pass: they write vertices and triangles into the
arena (`runtime/gfx/mesh_emu_spirv.h`) and the draw record's `DrawIndexedIndirect`, which the generated vertex shader
and the game's fragment shader then draw. The arena's counters are zeroed at the first emulated draw of each submission;
the draw records go round a ring of `MESH_EMU_DRECS`. `NX2SARIA_MESH_EMU_MB` sizes the arena (default 256 MiB),
`NX2SARIA_MESH_EMU_TASKS` its task records per submission (default 16384, 3 KiB each). Before the counters start over
they are copied to a readback ring; the perf log reports the most a submission asked for against the capacities.

- `ME`:
  - `VkPipelineLayout alayout`: the allocation shader's (push constants only).

### `me_alloc_pipe()`

The allocation shader's pipeline (`runtime/gfx/mesh_emu_spirv.h` `mesh_emu_alloc_spirv()`); 0 when it cannot be made.

### `me_arena()`

- `if (tbytes + (32ull << 20) > ((uint64_t)mb << 20)) mb = (int)((tbytes >> 20) + 64)`: room for vertices and indices.

### `me_barrier()`

The access masks also carry the generic `MEMORY_WRITE` / `MEMORY_READ` bits. `SHADER_STORAGE_READ/WRITE` exist only in
synchronization2 (bits above 31), and AMD's driver for Polaris (23.10.01, RX 580) did not act on them: the mesh dispatch
that reads the {groups, task workgroups, 1} the task stage had just written read zeros and launched no workgroup
(mesh.txt: "mesh groups started 0" with the record holding 32 x 12 x 1), so every draw with a task stage (Kokiri's moss
foliage, Link's door) drew nothing. Mesh draws without a task stage drew, because their record is read by the draw after
a command buffer boundary. The general replay barrier (`runtime/vk/vk_exec.c` `barrier_to()`) uses those bits.

### `me_bind()`

- `MeQ`:
  - `VkBuffer ib`: the first stage's indirect dispatch (`VK_NULL_HANDLE`: count).
- `MEQ`:
  - `uint8_t *d`: the push descriptors' data of the queued draws.

The stages of the emulated draws whose records were filled, waiting to be recorded (`me_flush`): the draws of a pass
with `NX2SARIA_MESH_EMU_BATCH`, which run in the part's pre-pass command buffer when it ends (`vk_exec_mesh_flush`, from
`runtime/vk/vk_sync.c`), or the one draw that runs in the part. Recorded by stage, with one barrier between stages for
all the draws: the fills, the allocation (task records; index ranges of draws without a task stage), every task stage,
the allocation of the other index ranges, every mesh stage. Each draw's indices are a range of its own and its task
records too (the allocation shader), so the draws' stages need nothing of each other's. One by one, as before
2026-11-11, each draw took three barriers, and its task base and first index were copies of the arena's counters after
the draw before it: an RX 580 spent 5.5 ms of a frame's GPU time on 69 draws' stages (Kokiri Forest), most of it waiting
at the barriers with the GPU nearly idle.

### `me_flush()`

The queued draws' stages into cb (the pre-pass command buffer, or the part for a draw that runs there); returns the
commands recorded.

- `if (MEQ.tasks)`: the task stages wrote the mesh dispatch {max mesh groups, task workgroups, 1} into the draw record.

### `vk_exec_mesh_flush()`

`runtime/vk/vk.h`: the pre-pass command buffer ends (`runtime/vk/vk_sync.c` `pre_end()`): the stages of the draws queued
for it go in first.

### `mesh_emu_draw()`

count: the first stage's workgroups, or ind: a {count, 1, 1} record to dispatch from args: an indirect draw's arguments
in game memory (count, 1, 1 copied into a ring entry, which ind then is).

- `NX2SARIA_MESH_EMU_BATCH` (default on): the stages run in the part's pre-pass command buffer (`vk_sync_pre`), which
  the queue runs before the part, and the pass is not split for them. At the first emulated draw of a pass the part is
  cut there (`vk_sync_cut`: ended, but handed to the queue only with the next part, so the texture pool set stays; a
  split there took a new set, and each took 2.3 ms of the replay to refresh), so that the pass begins the next part and
  only what came before it runs before the stages. Split, a pass of 20 emulated draws was 20 passes, each emulated draw
  a pass end and begin with a full barrier, three more barriers and the targets stored and loaded again: on an RX 580 6
  to 7 ms of a frame's GPU time went to the gaps between passes (`gpu_passes`, Kokiri Forest, 2026-11-10).
- `if (again.info != info)`: the pass again, in the new part.
- `if (!pre) vk_exec_barrier(ex)`: ends the pass; what the game wrote before (its culling) is visible to the stages.
- `if (!pre && MEQ.n) me_flush(vk_sync_pre())`: the draws queued for the pre-pass command buffer go before this one.
- `if (ep != ME.epoch)`: the first emulated draw of this submission: the counters start over.
- `if (ME.rb && ME.rb_map)`: the last submission's counters to the readback ring; one 32 resets back is read.
- `vkCmdFillBuffer(cb, ME.buf, 0, 32, 0)`: the counters, and the diagnosis words 4-7 (`runtime/gfx/mesh_emu_spirv.c`).
- `me_flush(vk_sync_pre())`: full, or not the next record (the allocation shader takes a run of them).
- The record: `DrawIndexedIndirect` {0, 1, 0, 0, 0}, mesh dispatch, task base, then the words the allocation shader
  reads (`runtime/gfx/mesh_emu_spirv.h` `MESH_EMU_R_*`); an indirect first stage's count is copied from its arguments.
- `vkCmdFillBuffer(cb, ME.buf, ro + MESH_EMU_R_TASK * 4, 8, tp ? 1 : 0)`: and the unused last word.
- `if (!pre) ex->recorded += 3 + me_flush(cb)`: the one draw's stages, here.
- The draw: prepared again (the pass reopens, every binding is recorded again).
- The index buffer and push constants are the emulation's: the next draw binds everything again.

### `bind_index_buffer()`

Binds a draw's index buffer unless the command buffer holds that bind already: the same buffer, offset and index type as
the indexed draw before, as when a mesh is drawn again with other uniforms or textures. B.ib is cleared with the rest of
B wherever the replay goes on in another command buffer or something else has bound into this one
(`vk_exec_draw_new_cb`, `cb_res`: a new submission or part, a chunk's own command buffer, the draws of `mo_emit` and
`mesh_emu_draw`, which bind index buffers of their own), so a bind from before those is never taken for held.

### `bind_index()`

Indices made here (a quad list or line loop as triangles or a strip, byte indices widened), from a place of their own in
the submission's transient memory.

### `op_draw()`

- `if (d->prim == NVN_DRAW_PRIMITIVE_QUADS && n >= 4)`: quads become two triangles each.
- `} else if (d->prim == NVN_DRAW_PRIMITIVE_LINE_LOOP && n >= 2)`: a strip that returns to its first vertex.

### `restart_for()`

Primitive restart of an indexed draw. NVN restarts strips wherever an index equals the SetPrimitiveRestart value; Vulkan
restarts only at the all-ones value of the bound index type, and only strips and fans without
primitiveTopologyListRestart. Byte indices are widened to 16 bits, so their restart value becomes 0xffff
(`op_draw_idx`). Without restart the 0xffff entries of the game's terrain and cliff strips fetch vertex 65535 + base and
the triangles around them drop out.

- `default: return 0`: lists: a restart index only drops the unfinished primitive.

### `quad_index_on()`

`NX2SARIA_QUAD_INDEX` (default 0): an indexed quad list is drawn as two triangles per quad and an indexed line loop as a
strip that comes back to its first vertex, from the game's indices (`runtime/vk/vk_index_rewrite.h`), as the non-indexed
ones are (`op_draw`). Off, they go to the GPU with the game's indices under the triangle list and line strip topologies,
and the first such draw is logged either way. Read once.

### `draw_idx_rewritten()`

An indexed quad list or line loop, drawn with indices made from the game's (`NX2SARIA_QUAD_INDEX`). 0: not one of those,
or left as it was (setting off, indices the CPU cannot read, a loop with primitive restart); the caller draws it.

- `if (quads && n < 4) return 1`: no whole quad: nothing to draw.

### `op_draw_idx()`

- `if (d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE)`: no uint8 index type here: widen (host-readable backing only).

### `op_draw_idx_ind()`

DrawElementsIndirect: the arguments are read by the GPU from the pool buffer (they are often written by an earlier
dispatch). The base instance stays out of c0 (0x644): c0 cannot be patched by a transfer inside a pass;
`gl_BaseInstance` of the Vulkan draw is the argument's. Byte indices need the count to be widened: skipped.

### Dispatch and uniform buffer updates

### `fmt_srgb()`

A compute dispatch whose shader cannot be translated: Switch 2 titles run tensor-core networks (HMMA/LDSM, an AI
upscaler: Ocarina's title movie goes 1920x1080 -> 2560x1440 through ten such passes) that the Maxwell translator has no
model for. Instead of leaving the outputs untouched (black), the largest bound storage image receives a linear blit of
the largest bound sampled RGBA8 texture that is a different image and no larger (the networks' 10-bit intermediates are
only partly meaningful as pictures, so they are not a source): what an upscaler approximates. In-place passes (the
output also sampled) are left alone. `NX2SARIA_CS_FALLBACK=0` turns it off. sRGB-ness of the RGBA8 formats the fallback
blits: a blit converts between encodings, the game's network writes the bits through a UNORM view, so a UNORM source and
an sRGB target go through a scratch image in the source's format (blit, scaling only) and then a raw copy.

### `fallback_scratch()`

Scratch images for those blits, one per (size, format), at most `SCRATCH_N`; a new size takes the place of the one used
longest ago (`runtime/vk/vk_scratch_lru.h`), whose image goes through the deferred destruction.

- `vk_tonemap_forget(cache[i].img)`: the stand-in's views of it, before a new image can take its handle.

### `uses_tensor_ops()`

1 when the bound compute program uses tensor-core instructions (HMMA 0x23c, IMMA 0x237, LDSM 0x83b): the game's
upscalers (the movie's and the 3D scene's), which the fallback blit stands in for unless `native_tensor` runs them.
Other untranslated programs (bindless texturing, indirect branches) bind the output image too, and a blit for them
pasted G-buffer normals and intermediates over the picture. Kept per program serial.

### `native_tensor()`

`NX2SARIA_NATIVE_TENSOR=1`: tensor-core programs run their translation (HMMA, LDSM and MOVM lowered to warp shuffles in
`tools/xlate/AmpereLowering.cs` Hmma, Ldsm, Movm). The lowering gives the hardware's results: its SPIR-V, run by the
driver, matches what the same SM86 kernels compute natively, bit for bit, on single instructions and on a network pass
over four warps (a check named `tensor_gpu_check.py`, which `tests/` does not hold). The infinities and noise the scene
upscaler wrote came from its input picture, not from the arithmetic (`net_binding`). Still off by default: no game run
has confirmed the picture since, and each network pipeline takes the driver tens of seconds and about 100 MiB of device
memory to build (`runtime/vk/vk_pipeline.c`, compute pipeline prebuild). While off, the programs take the fallback below
as untranslated ones do.

### `log_cs_bindings_locked()`

Everything an untranslated program binds (`NX2SARIA_LOG_CS_BINDS`): what a stand-in could use instead. Texture lock
held.

### `log_fallback_skip_locked()`

`NX2SARIA_LOG_CS_BINDS`: an untranslated program the fallback leaves alone, so its output keeps what it held (black when
nothing else wrote it): why, and what it binds. `in_place` -1: not a tensor program (its translation failed). Each
(program, output) once, 256 at most. Texture lock held.

### `fallback_9e5_scene()`

The scene upscaler's input as an RGB9E5 image, for an output in r11g11b10f: the post pass's picture (`raw_9e5`'s words).

### `cs_guest_hash()`

FNV-1a of a program's compute code as the game hands it over, kept per program: what `runtime/vk/vk_aa.c` knows the
game's SMAA passes by, the same with every translator and setting. 0 without compute code.

### `aa_dispatch()`

The anti-aliasing setting (`runtime/vk/vk_aa.c`): while it asks for something other than the game's SMAA, the game's
edge and weight passes are left out and its composition is replaced by `vk_aa_record`, from the same composite (the
sampled R11G11B10F image of the output's size) into the same scene target, through the integer storage view the game's
own pass writes it with. 1: the dispatch is handled (left out or replaced). `NX2SARIA_AA_LOG=1` logs each compute
program's guest code hash once, with its dispatch size and output, to find the passes in another game version.

- The scene target, written as raw words: the R11G11B10F image, or at times the RGB9E5 one the same memory also resolves
  as (`raw_9e5_twin`), which takes the same words.
- `if (storage_view_format(t, &v, hv) == VK_FORMAT_R32_UINT) dv = get_view(ex, t, &v, hv, 3, 1)`: 3: Texture2D.
- `for (int i = 0; dst && i < 128 && !src; i++)`: the composite: R11G11B10F of the same size, another image.
- `if (dv && sv) vk_texture_storage_bound_locked(dst)`: the GPU writes it below, as the game's pass would have.
- Left to the game, once logged. Expected while an area loads: the target is then the RGB9E5 image at the same memory,
  made without storage use, which the game's own pass cannot write either.
- Recorded without the texture lock: SMAA's first frame waits for its lookup textures' upload. The views stay valid
  meanwhile: images and views go only through the deferred release.
- `vk_exec_barrier_dispatch(ex)`: ends the pass; the composite's writes are visible.
- `ex->dc->B.pipe[1] = VK_NULL_HANDLE`: the replay must bind its compute pipeline again.

### `untranslated_dispatch_fallback()`

- `if (!uses_tensor_ops(ex))`: its translation failed, and no stand-in exists for programs without tensor ops.
- The output is looked for among the image slots the program uses: the count of image entries of its NVN2 control
  section (byte +0xb6), at least slot 0 (the networks' in-place passes count none and write slot 0). An image slot keeps
  its binding from earlier dispatches, so past that count lie other programs' images: with a 3840x2160 picture the scene
  upscaler leaves its 3840x2160 RGB9E5 target in slot 2, the largest image bound to the movie network's last pass, and
  that pass's real output (2560x1440, slot 1, which the frame's last passes enlarge) kept stale bits for the whole
  movie: the Triforce movie after Gohma showed as frozen coloured stripes.
- `for (int i = 0; dst && i < 128; i++)`: an in-place pass (its output also sampled) is left alone.
- RGBA8 sources (the movie's frames; its 10-bit intermediates gave black bands), or one in the output's own format: the
  3D scene (r11g11b10f) the game's upscaler enlarges to the output resolution.
- The picture also resolves as RGB9E5 at the same address; what the post pass wrote there stands in for it below.
  Refused for its format, the next input of its size (the older picture the upscaler blends with) was shown, and what
  moves trailed.
- `if (dst && src && knob_log_cs_binds())`: each distinct (program, source, target) once, 256 at most.
- `if (src && dst && fallback_9e5_scene(sf, dst->format))`: the image the post pass wrote this frame (`raw_9e5_twin`).
- `int native = raw_native_9e5(si, sf)`: RGB9E5 texels in an RGB9E5 image: decoded as they are, no copy.
- `VkImageView dst_view = VK_NULL_HANDLE`: for the tonemap pass, while the lock keeps the image.
- `dst_view = get_view(ex, dst, &nv, 0, 3, 1)`: 3: the translator's Texture2D.
- `static int e5_blit = -1`: E5B9G9R9 can be a blit source (with linear filtering).
- `if (e5 && !native)`: the raw words into a shared-exponent image (same texel size: a copy keeps the bits), then the
  blit.
- The square decode of the stored picture (`runtime/vk/vk_tonemap.c`). Builds of round 24 lost the device with it: its
  descriptor push did not go through the recorder (`runtime/vk/vk_rec.h` `rec_push()`; PORT-HANDOFF 7.12 item 11), which
  it now does. Its edge-adaptive filter is for the picture the screen shows: while DLSS or FSR 3.1 runs
  (`vk_dlaa_active`) that upscaler scales the scene itself and `vk_dlaa_upscaler` below puts its picture over this one,
  so the pass keeps its linear filter there, also in the few frames that upscaler misses.
- `if (tm)`: the tonemapped picture, already at the target's size, then the same blit as below.
- `ex->dc->B.pipe[1] = VK_NULL_HANDLE`: the replay must bind its compute pipeline again.
- The scene upscaler's input is the post pass's picture stored as the square root of each linear value (prog 102 packs
  `sqrt(composite)` into RGB9E5), and its output is linear: the HUD is drawn over it and the frame's last pass encodes
  to sRGB. A plain blit carried the stored values into the linear target, so the picture was washed out. The pass above
  squares them. Without it, this approximation decodes with the sRGB curve instead: scale into an RGBA8 UNORM scratch
  (the stored values), copy those bits into an RGBA8 sRGB scratch, and blit that into the target, which reads the sRGB
  image as linear (mid-tones a little dark, the darkest values a little lifted).
- `if (scratch)`: the bits as they are: a UNORM picture stays the same picture in an sRGB image's memory.
- `vk_dlaa_upscaler(ex, di, df, dw, dh)`: the scene upscaler's output: DLSS's or FSR's picture goes there
  (`runtime/vk/vk_dlaa.c`).

### `op_dispatch()`

- `if (aa_dispatch(ex, d->x, d->y, d->z)) return`: The game's SMAA left out or replaced (`runtime/vk/vk_aa.c`).
- A tensor-core program runs the stand-in while `NX2SARIA_NATIVE_TENSOR` is off, so ask that before translating it.
  Translating it anyway recorded four AI-upscaler compute shaders of 0.87-4.4M SPIR-V words that this game never
  dispatches natively, and building them cost gigabytes of memory at boot (`NX2SARIA_LIB_PREBUILD_BIG`, `bg_main`).
- `const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0)`: synchronous: a skipped dispatch changes
  results.
- `if (knob_log_cs_binds())`: diagnosis: the compute bindings of the first dispatch of each program.
- `if ((info->pool && !pool_on()) || shader_skipped(cs)) return`: `NX2SARIA_POOL=0`, `NX2SARIA_SKIP_SHADERS`.
- `vk_exec_barrier_dispatch(ex)`: ends the pass; earlier writes are visible to the shader.
- The replay's binding path is the CPU cost the frame profile needs to separate from recording.
- `if (vk_texture_deferred()) return`: past a parallel group: the op goes again once it is done
  (`runtime/vk/vk_exec.c` `op_again()`).

### `op_dispatch_ind()`

DispatchComputeIndirect: `vkCmdDispatchIndirect` from the pool buffer. The shader reads the group counts from c0 0x200,
so they are copied there on the GPU, into a c0 range no other dispatch uses, before the dispatch.

- `if (aa_dispatch(ex, 0, 0, 0)) return`: as in `op_dispatch`.
- `if (!native_tensor(ex)) { untranslated_dispatch_fallback(ex); return; }`: as in `op_dispatch`: never translate it.
- `if ((info->pool && !pool_on()) || shader_skipped(cs)) return`: `NX2SARIA_POOL=0`, `NX2SARIA_SKIP_SHADERS`.
- `cx->B.c0_valid[2] = 0`: a fresh c0 range: the copy below must not change one an earlier dispatch reads.
- The replay's binding path is the CPU cost the frame profile needs to separate from recording.
- `if (vk_texture_deferred()) return`: as in `op_dispatch`.
- `if (cx->B.c0_valid[2])`: the shader binds c0.
- NVN2 programs read the group counts from c0 byte 0 (`c0_info`); at 0x200 their bank holds pushed constants or the
  driver's tables, which the copy would overwrite.
- `cx->B.c0_valid[2] = 0`: the range no longer holds `B.c0[2]`.

### `op_update_ub()`

- `vk_exec_barrier(ex)`: earlier draws finished with the old contents.
- In a submission's tail (`runtime/vk/vk_exec.c` "the early tail") an update of a mirrored block reaches guest memory
  with the results the game reads, from the bytes here, and takes no write-back: one after the tail would land when the
  game is already writing the next frame's data. This goes the same way under the lazy write-back (`runtime/memory.c`
  "Lazy write-back", NVIDIA's and Intel's Windows default), which is what kept every tail of those drivers from going
  early: a write-back recorded here would run before the tail, whose copy makes the bytes, and carry the pool's older
  ones down over the guest's. It needs no write-back either: `ub_write_cb` delivers these very bytes
  (`runtime/nvn/nvn_api_cmd.c` records the payload) to the guest at the fence, and the tail's own copy keeps the mirror
  equal to them from then on; where the game passed its data through this very block, as it usually does, the guest
  pages hold them already. The one case left as before is a range the lazy write-back has armed
  (`runtime/vk/vk_memory.c` `vk_gpu_written_back()` arms what a shader wrote, and only that: blocks updated this way
  never get armed): there the fetch path owns the bytes, the update is noted as ever, and the tail goes after the
  write-backs. A range armed only in part passes and its fetch orders after the tail's copy, so it still brings these
  bytes down when a touch releases it.

### Bind ops

### `op_draw_impl()`

- `memset(ex->pipe.vattr, 0, sizeof ex->pipe.vattr)`: the new list replaces the old one.
- `else if (b->index >= 18 && b->index < 64)`: NVN2 has more banks than Maxwell's 18: say which, once each.
- `if (b->stage >= NVN_STAGE_TASK && b->stage <= NVN_STAGE_MESH && b->index >= 0 && b->index < 16)`: once per slot.
- `ex->tex_serial++`: the shader memo's handle check (`feedback_scan` reads only textures: a rescan at most).
- `cx->bind_gen++`: storage buffers may come from these words (`push_ptr`).

### `vk_mvobj_frame_masks()`

The last executed list's draws as " `<position` `banks>/<changed` `banks>:<draws>`" (bit n: cn), for
`NX2SARIA_MV_CHECK`'s lines.

### `op_is_draw()`

`NX2SARIA_REPLAY_TIME` (default 0, off): times each op so the frame-time summary can say how the replay splits between
draws, binds and transfers. The clock reads cost a fraction of a millisecond a frame, which is why it is off by default
and why the wrapper is the only thing between the walk and the switch.

### `vk_exec_phase_worker()`

- `enum { PK_DRAW, ... }`: `NX2SARIA_REPLAY_PHASES=1` (default 0, off; a developer switch): where the replay's time per
  op goes, apart for the submitting thread and the replay workers (whose chunks run beside it), by the op's kind and,
  inside a draw, by the phase of `prepare_draw_ex`: entry (the pass and the scissor), shader lookup, pipeline lookup,
  its bind and dynamic state, the per-draw hooks (DLAA, motion vectors, jitter, fixed state, repeat stats), the resource
  binding (of it the texture and buffer descriptors, the binding signature and the set packing and pushes, which
  `bind_resources` times itself), the vertex buffers and what follows, and the rest of the op (the index buffer, the
  `vkCmdDraw` call, a skipped draw). One line per thread kind every `PH_EVERY` frames (`vk_exec_phases_frame`). The
  clock reads cost about a tenth of a microsecond each, a dozen a draw, which the totals include.
- `ph_worker`: `vk_exec_phase_worker`: this thread is a replay worker.
- `ph_ns`: (`ph_t`, declared above `bt_now`: the last mark of the draw op being timed, 0 outside one).

  `__atomic`.

### `vk_exec_phases_on()`

- `ph_code_ns`: `__atomic`.

### `vk_exec_phase_seg()`

- `ph_seg_ns`: `__atomic`; the submitting thread's alone (`vk_exec_record` runs there).

### `vk_exec_op_draw()`

- `if (ph_on && k != PK_OTHER) ph_t = t0`: draws, dispatches and uniform updates mark their phases.

### `vk_exec_phases_frame()`

After each frame's end in replay order (`runtime/vk/vk_exec.c` `vk_exec_frame_end`): every `PH_EVERY` frames, the
phases' lines.

- By op code: the eight with the most time, and the walk's calls of `vk_exec_record` against the ops' sum.

### `vk_exec_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): the image view cache (under the texture lock), the spare
descriptor pools and transient chunks, and the continuations held for event order.

### `vk_exec_desc_mem()`

The memory report's descriptor pools (`runtime/vk/vk_memory.c` `outside_log()`): the replay's (`alloc_set`,
`REPLAY_POOL_SETS` sets each), the spare ones among them, and the texture pool's sets. Their memory is the driver's,
outside VMA, and has no size.

## `runtime/vk/vk_exec_xfer.c`

Transfers of the Vulkan op-stream replay: buffer and texture copies, texture clears, event signals, counter reports and
conditional rendering. `runtime/vk/vk_exec.c` calls `vk_exec_op_xfer` for these ops.

Buffers are guest memory: `vk_mem_resolve` maps an address to the `VkBuffer` behind its pool, textures are the
`runtime/vk/vk_texture.c` images (linear texel layout, GENERAL layout; block-linear storage is only converted when a
mirrored texture is uploaded). So a copy is one Vulkan transfer command and the pitch of a buffer side comes from the
payload (`OP_COPY_STRIDE` or the tight row). Every copy ends the open pass and starts with a full barrier. Destinations:
an image is marked GPU-written at record time; a CPU-visible buffer range is reported to `vk_gpu_written` once the GPU
is done (write watch does not see GPU writes, textures stored there re-upload).

Events and counters are CPU-visible side effects. Their values go into guest memory on the completion thread through
`vk_completion_add`, in submit order, as `runtime/gfx/gfx_null.c` does: event WRITE / ADD, counter {0, timestamp} for
every type but samples passed. A timestamp report puts 0 in the counter word: the game gates its clears on two such
words being unequal.

Samples passed (`NVN_COUNTER_TYPE_SAMPLES_PASSED`) is a running count on the queue: `RESET_COUNTER` zeroes it, every
draw after it adds the samples that passed the depth and stencil tests, `REPORT_COUNTER` writes the running value. The
game resets and reports it 3 to 8 times a frame in the field and its CPU reads the results back (`f_70010c3298`); a 0
there culls what the query tested. The replay counts with Vulkan occlusion queries ("occlusion counts" below).

Conditional rendering is a CPU-side skip, not `VK_EXT_conditional_rendering`: the extension compares one 32-bit word
with zero, NVN compares the counter words at addr and addr + 16. The condition is evaluated when the op is recorded from
guest memory; a counter reported by an earlier submission that is not done yet counts as 0 if it is a timestamp, as
unknown if it is samples passed. An unknown count renders: the report usually sits in the command buffer being recorded,
so its value cannot be waited for, and the word in memory still holds an older query's count. Rendering costs at most
the work the condition would have saved; in the field the game conditions only on timestamp reports. The condition sets
`VkExec.render_disabled`, which `runtime/vk/vk_exec.c` honours for clears and `runtime/vk/vk_exec_draw.c` for draws and
dispatches. A condition that cannot be read (address outside every pool) renders. Copies and `CLEAR_TEX` are not
conditional.

### Completion callbacks (completion thread)

- `Range`: back: written into a VRAM mirror (`vk_mem_mirrored`).
- `ex_results`: What the GPU work of the submission being recorded hands to the CPU at its completion without a fault to
  fetch it: event values, counter and occlusion reports, GPU writes of memory no VRAM mirror holds
  (`vk_sync_recorded_wait` waits for the newest such submission). Write-backs of the mirrors note it in
  `runtime/vk/vk_memory.c`; the lazily written ones fetch on the CPU's first access.

  `VK_RES_*` bits.
- `tail_results`: those noted while the submission's tail records (`runtime/vk/vk_exec.c` "the early tail").

### `vk_exec_note_results()`

- `vk_gpu_passes_result(kind)`: `NX2SARIA_GPU_PASSES`: where in the submission it is done.

### `ub_write_cb()`

An inline uniform update in a submission's tail (`runtime/vk/vk_exec_draw.c` `op_update_ub()`, `runtime/vk/vk_exec.c`
"the early tail"): the GPU copies the bytes into the pool's mirror where the stream has the update, and guest memory
gets them here, with the results the game reads, in place of a write-back.

### `note_gpu_write()`

The GPU wrote [addr, addr+size) of CPU-visible memory: tell the texture cache once the fence is reached.

### `vk_exec_late_stats()`

The side effects below that ran after a waiter on the executed timeline had gone on
(`runtime/vk/vk_sync.c` `vk_sync_effect_late()`): the game was already past the wait after its submit when these reached
its memory.

### `write_counter()`

`NVNcounterData` = {counter, timestamp}; timestamps are Tegra 19.2 MHz ticks (the base of GetSystemTick). keep: the
4-byte quarters of the record the write leaves as they are (bit n: bytes 4n to 4n + 3), 0 none. `ahead_ns`: the
timestamp is that much past now (a tail timestamp the GPU has not reached yet, `counter_cb`), else 0.

### `counter_cb()`

- `report_write()`: Reports other than samples passed (timestamps; the statistics counters are not modelled) write a
  counter of 0. The counter word matters: Ocarina writes {1, 0} into a record from the CPU, reports `TIMESTAMP_TOP` into
  it and gates clears on it differing from a zero record (mode 1), clears that must not run.

  below, with the pending table.
- `CounterWrite`: tail: the submission's value for a timestamp reported in its tail (`runtime/vk/vk_exec.c` "the early
  tail"), else 0.

- A timestamp of the tail, written while the GPU still runs the tail: the time the tail is expected to end at.

### `vk_exec_cond_frame_end()`

- `PENDING`: Reports queued by the replay that the GPU has not finished. The condition of `RENDER_COND` reads a
  timestamp one as 0 and a samples-passed one as unknown. The newest report per address, in a table probed over
  `PENDING_PROBE` slots from the address's hash: Ocarina issues hundreds of occlusion reports a frame and conditions
  draws on them in the same frame, so a ring of the last 256 reports had dropped the report a condition referred to,
  which then read an older query's count (often 0) and culled visible ground and trees. It also rotates through tens of
  thousands of record addresses, so an entry whose value is in memory (written >= seq: a condition reads memory anyway)
  gives its slot to a new address; a table that only grew filled up after a minute and conditions on the newer records
  read the CPU's 1 again.
- `Pending`: seq: the newest report to addr; written: the newest whose value the completion thread has put in guest
  memory. A report counts as pending until then, not until its fence passes: the callback runs after the fence, and in
  between the record still held what the CPU wrote (Ocarina re-arms its condition records with 1 for the next frame), so
  a condition read 1 where the GPU had written 0, a depth clear ran after the depth copy and frames showed only sky.
  frame: the present count (`vk_exec_cond_frame_end`, in replay order) when the report was replayed. The game arms its
  condition records from the CPU between frames, so within a frame a timestamp report decides the record's value even
  after it reached memory: Ocarina's CPU, running a frame ahead, had re-armed a record with 1 by the time a later depth
  clear of the frame read it, the clear ran after the depth copy and the frame showed only sky (white flashes).
  `clr_seq`, `clr_mask`: the quarters of the record a small buffer clear filled after the report `clr_seq`, the newest
  replayed before the clear ("small buffer clears over counter records" below). Written by the replay thread under
  `clr_lock`; the report writes read them under it.
- `vk_cond_clears_run`: Per-frame counts of conditional work (`NX2SARIA_LOG_RCOND=1`, `vk_exec_cond_frame_end` logs
  them): clears and draws issued under a render condition, run or skipped.

### `pending_written()`

Completion thread: the value of report seq to addr is in guest memory.

### `pending_slot()`

Lookups run on the replay thread and the completion thread; only the replay thread inserts.

- `victim = e`: done, and not from this frame (whose reports still decide their records).
- `if (!e) return NULL`: every probed entry still has a report in flight.

### Small buffer clears over counter records

(`NX2SARIA_CLEAR_SMALL_CPU`, default 1; 0: the GPU's fill alone).

A ClearBuffer or ReportValue is a fill the GPU makes where it stands in the stream (`clear_buf`): guest memory has the
bytes once the GPU got there, through the imported pool, a mirror's twin or the mirror's write-back. A counter report
reaches guest memory another way, as a CPU write after its submission's fence (`report_write`), and a render condition
reads the record when it is replayed (`counter_word`). For a record that gets both, the two ways do not keep the order
of the stream:

- A report followed by a clear ends as the report: its write comes after the fence, the fill ran before it. When the
  clear is in a later submission the completion thread races the GPU for the last word;
- A condition replayed after the clear reads what memory held before the GPU ran the fill, or the 0 of a report that the
  clear has since overwritten.

So a fill of at most `CLEAR_SMALL` bytes is also applied here, at its place in the replay, to the records it covers that
the pending table holds, which are the records a counter report went into: the covered bytes get the value in guest
memory at once, the entry notes which quarters of the record were filled and after which report, reports up to that one
leave those quarters alone when their write comes, and a condition takes them from memory. A report replayed after the
clear writes the whole record as before. The GPU's fill is recorded all the same; it writes the value a second time and
is what a shader or a mirror sees. Only those records, not every small fill: the write here is ahead of the GPU, and the
GPU reads guest memory too, at once in an imported pool and through the next upload of the page the write watch then
lists in a mirrored one. A draw count or an atomic counter a shader adds to, reset by such a fill, would read the reset
before the passes that still count on the old value have run. Nothing on the GPU reads a counter record (conditions are
decided on the CPU), a report's own write already lands on the same page, and whatever this write puts a value over, the
GPU's fill puts the same value over later. The counts (`vk_exec_clear_stats`) are kept whatever the setting is: the
small fills into CPU-visible memory, how many covered such a record, and the reports that went into bytes one of the
last `CLR_RING` fills had covered.

- `clr_lock`: a report's write against a clear's note and write.
- `clr_any`: an entry holds a note: the report writes take `clr_lock` from here on.
- `clr_cpu`: the setting.
- `rep_pages`: A flag per 4 KiB page a counter report went into, by a hash of the page (a page that shares another's
  flag only costs a clear its table lookups). Replay thread.
- `CS`:
  - `uint64_t n, bytes, hit, recs, after, frame0`: since the last line: fills, their bytes, fills over a record, such
    records, reports into filled bytes; the frame of the last line.
  - `volatile uint64_t kept`: report writes that left filled quarters alone.
  - `uint64_t addr`: the last fill.
  - `struct { uint64_t lo, hi; } ring[CLR_RING]`: the last fills, and the range that holds them all.

### `clear_note_report()`

Replay thread, at a report into the record at addr.

### `clear_small()`

Replay thread: the fill of [a, a + n) with value, 4-byte aligned, backed and CPU-visible, at its place in the stream.

- `if (!*rep_page(a) && !*rep_page(a + n - 1)) return`: no report ever went into these pages.
- `for (uint64_t k = a - 12; k < a + n; k += 4)`: every record address whose 16 bytes reach into the fill.
- The quarters filled since the newest report: an earlier fill's stay when no report came in between.
- `e->clr_seq = report_seq`: every report replayed so far.

### `clear_quarters()`

The quarters of e's record that a small clear filled after its newest report: memory holds them.

### `report_write()`

Completion thread, or the thread whose wait runs the results: report seq to addr goes into guest memory, but for the
quarters a small clear replayed after it has filled.

### `vk_exec_clear_stats()`

The counts since the last call as a clause for a periodic log line (`runtime/vk/vk_usnap.c` summary). Replay thread.

### Occlusion counts (samples passed)

After a `SAMPLES_PASSED` reset, the first draw of a pass begins an occlusion query (precise when the device has
`occlusionQueryPrecise`); the query ends with the pass, or at the next report or reset of the counter, and the next
draw begins another. A Vulkan query begun inside a render pass must end inside it. The game always resets, draws the
tested proxies, reports, so counting stops at a report and restarts at the next reset: queries cover only the draws
between the two, not every pass of the frame. Once the game reports twice without a reset in between, counting stays on
for good (cumulative NVN semantics; that one report misses the draws since the previous one). The queries closed since
the last report or reset form a segment, handed to the completion thread with that report or reset: in submit order it
sums the segment's results into the running count (a reset drops the sum and zeroes the count) and writes the count of
a report into guest memory. Slots come from one ring; the completion thread reads and host-resets them
(`hostQueryReset`, Vulkan 1.2), so the replay records no reset commands. A full ring (no slot read back yet) leaves the
draw uncounted and the segment's count at least 1, as does a query still unavailable after its fence. Without
`hostQueryReset` the counts stay 0. The ring holds about four frames of Hyrule Field's queries (about 3,500 a frame):
the replay workers' chunks take blocks of slots sized to their draws (`vk_exec_xfer_slots`), more than they use, and
with 4096 the ring came round to slots the completion thread had not reset yet, leaving draws uncounted.

- `oq_state`: 0 not set up, 1 counting available, -1 unavailable.
- `oq_busy`: from begin until the completion thread read and reset the slot.
- `oq_next`: replay: next ring slot.
- `oq_counting`: replay: draws are counted (a reset since the last report, or `oq_keep`).
- `oq_reported`: replay: a samples-passed report since the last reset.
- `oq_keep`: replay: the game reported twice without a reset: count every draw.
- `oq_open`: replay: the slot whose query is active in the open pass.
- `oq_seg`: replay: slots closed since the last report or reset.
- `oq_lost`: replay: the segment missed a draw (ring full).
- `oq_count`: completion thread: samples since the last reset.
- `XW`: The replay workers (`runtime/vk/vk_exec.c` "parallel groups") record the draws of one group at once. The front
  pass replays a group's samples-passed resets and reports in the order of the stream before the workers record
  (`vk_exec_xfer_front`): each makes its segment, queued for the completion thread in that order, and they are listed in
  GS. A worker follows the counting as the stream does from its chunk's first op (`vk_exec_xfer_chunk_state`):
  XW.counting and the segment its closed queries belong to, the next one at or after its position in the stream (XW.seg;
  past the group's last, the pending list `oq_seg`, which a later reset or report takes). It keeps the query open in its
  own pass (XW.open), takes ring slots from its chunk's block and keeps the slots it closes with their segment in its
  chunk's list (CP), which `vk_exec_xfer_group_merge` puts into the segments once the group is done, chunk after chunk:
  under a lock per slot, the workers waited for each other at every query. XW.cond: the decisions of its chunk's render
  conditions, made by the front pass in the order of the stream. The blocks (`vk_exec_xfer_slots`): the front pass gives
  each chunk as many slots as it has draws that count samples and render, one after the other, so the slots follow the
  stream as the replay's own do and the completion thread reads and resets them in long runs (`oq_take`). Taken from the
  ring at once, the slots of a group's chunks interleaved: four times the result calls and a reset per slot, 3.5 ms more
  of the wait after a frame's first submission (RX 7900 XT, Hyrule Field, 3 workers). A chunk that needs more than its
  block takes them from the ring.
- `GS_GROUPS`: GS: per group slot (`runtime/vk/vk_exec.c` `LIVE_GROUPS()`), the front pass's segments of the group.
  tail: with `NX2SARIA_REPLAY_PIPE` the submitting thread replays on past the group before its chunks are done: the
  first segment made after the group's front pass (`tail_wait`), by the next group's front pass or the replay, takes the
  queries the chunks close after the group's last report or reset, which in the stream come before that one's own.
- `gs_on`: the group slot whose front pass runs.
- `CP`: per chunk: closed slots and their segment.

### `vk_exec_xfer_group()`

A group's front pass begins (its slot), or it ended (-1): the list stays.

- `else if (gs_on >= 0) GS[gs_on].tail_wait = 1`: until `vk_exec_xfer_group_merge`.

### `vk_exec_xfer_slots()`

The workers of a group the submitting thread went on past take some too.

### `seg_add()`

Adds closed slot q to the segment a worker's queries belong to now, or the pending list.

### `oq_ready()`

- `runtime/vk/vk_device.c` enables every feature the device reports, so supported means enabled.

### `vk_exec_oq_draw()`

- A worker takes the next slot of its chunk's block, past it one from the ring at once (as the replay does, while a
  group's workers may still take some); a busy one is lost, not tried again.

### `vk_exec_xfer_shutdown()`

- `oq_batch_drop()`: the results kept by the completion thread (`oq_take`).
- `oq_hold_drop()`: the counts remembered by the completion thread (`oq_report_count`).

### `oq_batch_drop()`

- `OQB`:
  - `uint32_t first, n`: the slots kept: [first, first + n).
  - `uint64_t v[OQ_BATCH * 2]`: per slot: samples, available.
  - `uint32_t r0, rn`: the run of taken slots still to be reset.
  - `uint64_t calls, slots, segs`: since the last perf line: result calls, slots they read, segments served.

The completion thread reads the results in batches (`NX2SARIA_OQ_BATCH`, default 1; 0: every segment by itself, as
before). The game tests one proxy per report, so a segment holds one query, and a frame of Hyrule Field has about a
hundred of them: one `vkGetQueryPoolResults` and one `vkResetQueryPool` each. On an RX 7900 XT (26.10.1) each of those
calls goes into the kernel, and the two hundred took 3.9 ms of every frame on the completion thread, which runs a
submission's callbacks before it lets the submission's waiters go: the game's main thread waits for the previous frame's
fence before it lets the next frame's GPU work start, so the frame after one replayed inside its submit call began 4 to
6 ms late with every thread idle. Slots come from the ring in order, so the slots of the segments that follow lie behind
the one asked for: one call reads `OQ_BATCH` slots from there on with their availability, the available ones are kept
(the results stay in the pool until the reset), and the segments after it take theirs from what is kept. A slot not
available in the batch (its part had not run yet) is read again when its segment comes. The slots taken are reset in
runs, one `vkResetQueryPool` for each: a run ends when a slot that does not continue it is taken or a new batch is read.
Until then its slots stay busy for the replay, at most `OQ_BATCH` of the ring's `OQ_N`, also while the game reports
nothing.

### `oq_take()`

The samples of slot q, whose submission has completed; `*avail` 0 when the query has no result.

- Not kept, or kept as unavailable (read before its part ran): from this slot on, up to the replay's next slot.
- `oq_reset_flush(dev)`: before the kept results go: the run's slots are among them.

### `oq_hold_drop()`

`NX2SARIA_OQ_HOLD` (default 0): a report whose segment left draws uncounted (a full ring, `vk_exec_oq_draw`) writes the
larger of what was counted and the last complete count of its record (`runtime/vk/vk_oq_hold.h`); off, what was counted,
at least.

- Such reports are counted either way and logged at the 1st, 10th, 100th... so a log shows how often the ring ran.

full. Completion thread only, as `oq_count`.

### `oq_cb()`

- A segment the replay workers filled lists its slots in the order the chunks closed them: in ring order, they are taken
  in runs (`oq_take`).
- The fence has passed, so a query that is still unavailable will not become available: waiting would block the
  completion thread, and with it every later callback, for good. It counts as visible (one sample).
- A query its submission never wrote. Every RX 6000 report of a lost device (0.8.0 to 0.9.2) had this line first, its
  callback run for work the GPU had not finished. A timeline read above every value its signals carry, then the device
  itself, end the run here when it is lost (`runtime/vk/vk_sync.c` "a lost device in the timelines' values",
  `runtime/vk/vk_device.c` `vk_device_lost_poll()`). Otherwise the slot counts as visible and the game goes on, as a
  ring that came round too early would leave it. The first such slot is logged with the submissions' state and the GPU's
  breadcrumbs: whether the GPU had finished this submission, and whether the timeline read above what was signalled.

### `oq_segment()`

Hands the segment to the completion thread with a report (addr; 0: no CPU-visible memory) or a reset.

- `if (gs_on >= 0)`: the front pass: the workers add the queries of the group's ops before it.
- `for (int g = 0; g < GS_GROUPS; g++)`: the first made after a group's front pass.

### `oq_report()`

A samples-passed report: the segment goes with it (addr 0: no CPU-visible memory) and counting stops until the next
reset, unless reports came without resets before.

### Event order check

The game orders work between its queues with events: a command buffer waits (WaitEvent) until another's SignalEvent has
set the value. Every submission replays here on one queue in the order it came, and a wait records nothing, so a wait
whose signal comes in a later submission does not hold its work back: that work runs before what it waits for. The
replay keeps the value each event reaches in replay order (`ev_seen`, the signals recorded so far, on top of the value
in memory) and counts the waits it reaches unmet; `NX2SARIA_TRACE=perf` logs the count, and the first few are logged as
they happen. Replay thread only, apart from `vk_exec_event_met_now`'s unlocked read of `ev_seen`.

- `EV_OPEN`: Waits reached unmet, until a replayed signal meets them (`ev_late`: the signal came that many submissions
  later, so the work after the wait ran before the work it waited for) or they are pushed out by newer ones.

### Events the CPU signals

Ocarina gates its main queue on the CPU: each frame the game sets one of two events to 0 (they take turns frame by
frame), submits the worker queues' command buffers and the main queue's, whose first command waits for the event to be
1, and signals 1 from the CPU (`nvnEventSignal`) once it has finished writing what the frame's commands read. The signal
comes before the main queue's submit call, after it, or while that call is replaying. On the console the GPU starts the
queue at the signal and reads memory from then on. Here a submission's recording begins with the copies of what it
reads: the VRAM mirrors' sync and the uniform snapshot (`vk_exec_record`), and the wait is tested when the walk reaches
it, some hundred microseconds later. A signal that came in between passed the wait, and the frame was drawn from copies
taken before the game had finished writing: single frames with the scene's shadowing missing or the whole scene black,
the interface as it was, most of them when other work keeps the CPU busy, since the game's threads then finish later and
the signal falls into the replay more often. So a wait is tested against the value the event had when the submission
took its copies: every `nvnEventSignal` is numbered and keeps the value it replaced (`vk_exec_event_cpu`, before the
write), `vk_exec_record` notes the number at its start (`vk_exec_event_sync`), and a signal with a later number does not
count for that submission. The wait then stops the submission like any wait reached before its signal, and
`runtime/vk/vk_backend.c` `submit_sync()` lets it go on at once in a submission of its own, whose copies are taken after
the signal. `NX2SARIA_EVENT_AT_SYNC=0`: the value in memory when the walk reaches the wait, as before.

- `cpu_ev_n`: `cpu_ev_lock`.
- `cpu_ev_seq`: `cpu_ev_lock`: `nvnEventSignal` calls so far.
- `ev_sync_seq`: `cpu_ev_lock`: `cpu_ev_seq` at the recording's start; `UINT64_MAX` outside one.
- `ev_after_sync`: replay: waits only a signal after the copies would have passed.

### `vk_exec_event_cpu_known()`

1 when the CPU has signalled this event before (`nvnEventSignal`): its waits are gates the game opens from the CPU.

### `vk_exec_event_holds_now()`

The wait's condition on the value in memory now, whatever the replay has seen. Any thread.

### `ev_mem()`

The event's value in memory, and in `*at_sync` the value it had when the submission being recorded took its copies (the
same outside a recording, and for an event the CPU has not signalled since).

- `for (unsigned k = 0; k < n; k++)`: the earliest signal after the copies replaced the value they saw.

### `ev_met()`

Whether the wait holds. `*late`: it holds only by a CPU signal that came after the submission's copies.

### `vk_exec_event_met_now()`

The value in memory now, or the signals replayed so far, whatever submission is being recorded: for
`runtime/vk/vk_exec.c` `vk_exec_held_ready()`, which asks without the submission lock whether held work can go on in a
submission of its own. That submission takes its copies later, so a CPU signal given before now counts for it; testing
at the copy point (`ev_mem`) would take the copy point of the recording another thread may be making. `ev_seen` belongs
to the replay and is read here without its lock, so the answer is a decision only: a wrong "ready" makes an empty
submission, which decides again under the lock (`runtime/vk/vk_exec.c` `evo_resume()`).

### `report_counter()`

- `report_seq++`: before `oq_report` / `counter_cb` take it.
- `if (nvn_gpu_only(p->addr))`: CPU-invisible pool: no host memory behind it.
- A timestamp in the tail is the one result a tail may hand the CPU: its time is written ahead (`counter_cb`).

### Conditional rendering

### `counter_word()`

1: `*v` read, 0: not readable, 2: a samples-passed report not in memory yet (unknown).

- `const Pending *e = pending_slot(addr, 0)`: the last report to addr decides.
- But for the halves of the word a small clear filled after it (`clear_small`): memory holds those.
- `int zero = 0`: the report decides the halves that were not filled: 0.
- `if (!e->unknown && e->frame == cond_frame) zero = 1`: a timestamp/statistics report of this frame.

### `last_type()`

The counter type of the newest report to addr the ring still holds, -1 when there is none (`NX2SARIA_LOG_RCOND`).

### `vk_exec_cond_why()`

`NX2SARIA_LOG_RCOND=ab` (diagnosis): the words a condition compares and where each came from: "f" a report replayed this
frame (0), "p" a pending report (0 or unknown), "m" memory, "c" memory after a small clear that came after the report
(`clear_small`), with the newest report's type and frame.

### `cond_enabled()`

- `if (rx == 2 || ry == 2) return 1`: a samples-passed count not done yet.
- `NX2SARIA_LOG_RCOND=1`: one condition in 499 (up to 300 lines), on or off, with the counter types last reported there.
- (pending: the report value has not reached guest memory yet).
- `int px = 0, py = 0`: the word came from a report not done yet (forced to 0) rather than memory.

### Textures

- `Sub`:
  - `int32_t x, y, z`: region origin.
  - `uint32_t w, h, nz`: region extent; nz: slices (3D) or layers.

### `sub_resolve()`

Resolves tex, its view and a copy region to image coordinates. 0 (warned) when the copy cannot be replayed.

- `int k = img->scale > 1 ? img->scale : 1`: a larger shadow map: the game's region in its texels.

### `buffer_side()`

Buffer side of a texture <-> buffer copy: bytes covered, pitches, Vulkan buffer span (a VRAM mirror when the pool has
one).

### `copy_buffer_image()`

- `if (s.img->scale > 1)`: the buffer holds the game's texel count, the image more.

### `copy_t2t()`

- Otherwise a scaled or converting copy: nearest, or linear when asked for (colour only).

### Buffer to buffer

### Buffer fills

(ClearBuffer, and ReportValue as a 4-byte one): `vkCmdFillBuffer` in GPU order after a full barrier, so the value lands
after earlier work, as NVN's semaphore release does; CPU-visible ranges go to `vk_guest_written` at the fence, as copies
do. Works in CPU-invisible pools. A small fill of CPU-visible memory is also applied to the counter records it covers,
here in the replay (`clear_small`).

### `clear_buf()`

- A VRAM mirror's guest copy is filled too, at once: atomics through the device address table count in both copies
  (translator patch 0036), and a counter reset here must start from the same value in both.

### Texture clears

### `clear_tex()`

Whole levels only (`vkCmdClear*Image` has no rectangle); partial regions, channel masks and a view that reinterprets the
format are skipped.

- A value of the other kind that is all zero bits (0.0f is) is the same clear either way: Ocarina clears its rg16ui edge
  map (480x270) with a float 0 each frame, and skipped, the post chain's edge classification read the previous frames'
  edges. Other values stay skipped.

### Parallel groups

(`runtime/vk/vk_exec.c`): the CPU side in the front pass, the rest in the workers.

### `vk_exec_xfer_front()`

The front pass, in the order of the stream, before the workers record (`vk_exec_xfer_group` on): a counter report or
reset (completion callbacks, pending entries, the occlusion segments and counting), a render enable and a render
condition (decided here, on the reports replayed so far). Returns the condition's decision: 1 renders.

### `xfer_worker()`

A worker: what the front pass left, in its chunk. The contact shadows' pass (`vk_contact_flush`) is recorded where the
replay would, once every earlier chunk of the group is done.

- `oq_close(ex)`: into the segment this op ends; the next queries go to the next one.
- `if (XW.reported) XW.keep = 1`: as `oq_report`.

### `op_xfer_impl()`

- `case OP_COPY_T2B: copy_buffer_image(ex, payload, 0); break`: `p_copy_b2t`: src holds the destination address.

### `vk_exec_op_xfer()`

`NX2SARIA_REPLAY_TIME`: see the note over `vk_exec_op_draw` in `runtime/vk/vk_exec_draw.c`; transfers are the third
kind.

## `runtime/vk/vk_exec_probe.c`

nx2saria.exe --probe-vkexec: the Vulkan replay, through `gfx_vulkan_backend`. Sync part: empty submissions get
consecutive fence values; `fence_wait` and `fence_done` agree; completion callbacks run in submit order on the
completion thread; a guest-memory word a callback writes is visible once `fence_wait` returns; finish drains every
submission; a present of a cleared `runtime/vk/vk_texture.c` image (layout GENERAL) reads back exactly through the
swapchain capture. Clear part: a hand-built op stream (`SET_RT` to an RGBA8 and a D32 texture, whole-target clears, then
scissored clears) through submit reads back exactly with `read_texels`. Draw part (`test_draw()`), pass marks part
(`test_marks()`: the GPU's buffer marks and the ring records name the last pass of a submission, and the device-lost
report text says so) and xfer part (`test_xfer()` and the tests after it) are described at their functions.

### `test_sync()`

1. `SUBMITS` submissions of an empty recording as the front end hands it over ({`OP_BEGIN`, `OP_END`}; every third with
   no handle at all). Each one's callback (`note()`, which runs on the completion thread and counts a run on the
   submitting thread as a failure) is queued before the value exists, as `runtime/vk/vk_exec.c` does. Every tenth is
   waited for, and by then exactly its callbacks ran.
2. One submission whose callback writes a guest-memory word: the word is visible to the waiter once `fence_wait`
   returns.
3. `TAIL` more, then `finish`, which drains everything.

The callbacks must have run in submit order; the word submission sits between the two runs, which is why the expected
value skips one after `SUBMITS`.

### `test_present()`

The image is marked as GPU-written (`vk_texture_gpu_written`) so that the clear is its content, not the storage. The
exact colour check needs the bilinear blit: FSR's approximate reciprocals move a flat colour by 1/255. `finish` submits
the queued present and, with capture on, reads it back. The picture is compared inside its rectangle (`gfx_fit_rect`);
around it, when the window's shape differs, the bars must be opaque black (`runtime/gfx/gfx_present.c`), which is the
same word in both byte orders.

### `test_clear()`

The clear part: a hand-built op stream through submit, read back with `read_texels`. `SC_X`, `SC_Y`, `SC_W` and `SC_H`
are the scissor rectangle: a pixel of the RGBA8 target after the stream holds the scissored second clear inside it and
the first clear elsewhere.

The textures are static because the payload's texture pointers must live as long as the submission. The first colour
clear covers the whole target outside a pass (`vkCmdClearColorImage`), the first depth clear likewise
(`vkCmdClearDepthStencilImage`); the second pair is scissored, so it takes a pass and `vkCmdClearAttachments`. The last
`OP_SCISSOR` leaves the replay's scissor unbounded for later submissions.

### `dr_prog()`

A program of one census shader as the game hands it to NVN: control section, code with its 0x30-byte header.

### `draw_prefix()`

The draw part's stream up to the draw: target cleared, viewport, program, three RGBA32F vertices at 0x80000 of the pool,
identity uniforms at 0x90000.

### `test_marks()`

The pass marks part (`runtime/vk/vk_crumbs.c`): one submission with a pass of 2 draws into the draw part's target, a
dispatch and a pass of 3 draws into a second target. Once its fence is done, the buffer marks must name the last pass as
both started and finished, the ring records must hold what the three were, and `vk_pass_marks_format` (the text
`runtime/vk/vk_device.c` `device_fault_report()` logs) must name it. Without `VK_AMD_buffer_marker` or with
`NX2SARIA_PASS_CRUMBS=0` it is skipped: NVIDIA's checkpoints can only be read once the device is lost.

### `test_draw()`

The draw part: the pipeline self-test, then a hand-built stream that clears an RGBA8 target, binds a census vertex +
fragment pair and draws one triangle; a census compute shader is dispatched. The census (`probe_census_dir`,
`tools/shader_census.py`) is read the way `vk_pipeline_selftest` reads it: a pair is a fragment shader and a vertex
shader of the same source file. `layouts_equal` counts the texels compared equal between the two layouts (0: not
compared); `ind_draw_texels` and `ind_dispatches` the indirect draws' texels compared and the indirect dispatches run.

- Vertex data at 0x80000: three vertices of `DR_ATTRS` RGBA32F attributes, attribute 0 the position. Uniform data at
  0x90000: identity matrices back to back, so that a vertex shader multiplying the position by one passes it through.
- With pipeline libraries (`vk_caps.gpl`) the same draw goes through the shared set 0 layout too and must be
  texel-equal.
- DrawElementsIndirect (`OP_DRAW_IDX_IND`) with the pair that drew: indices {0, 1, 2} at 0xA0000; the arguments are
  copied by the GPU (`COPY_B2B`, barrier) to 0xA0200 right before the draw, so the replay must read them on the GPU. {3,
  1, 0, 0, 0} draws the same triangle (texel-equal to the direct draw); count 0 draws nothing.
- Compute: a few census compute shaders, one dispatch each, resources unbound (zero buffers and stand-in images).
- DispatchComputeIndirect: group counts {1, 1, 1} copied by the GPU to 0xA0500 first.

### `test_xfer()`

The xfer part: copies, a texture clear, an event and a counter through submit. `xf_tex()` makes a texture in the xfer
pool at `off`.

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

The downsample's `p_copy_t2t` is filled as `nvnCommandBufferDownsample` records it.

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

A pool of zero bytes (the game makes some now and then) initialises as an empty pool: no Vulkan buffer or memory (a
buffer or import of 0 bytes is invalid usage) and no pool index entry, so a lookup at its address still finds the pool
that starts there, before and after the empty pool's Finalize.

### `probe_vkexec()`

HDR is turned off first (`vk_hdr_off`): the present checks read 8-bit swapchain pixels. At the end the pipelines, the
draw part's stand-in resources (`runtime/vk/vk_exec_draw.c` `vk_exec_draw_shutdown()`) and the occlusion query pool
(`runtime/vk/vk_exec_xfer.c` `vk_exec_xfer_shutdown()`) go before the device, which must be idle for both.

## `runtime/vk/vk_rec.c`

The recorder thread; what it is for is under `runtime/vk/vk_rec.h` below. One producer (the submitting thread, between
`rec_begin()` and `rec_end()`) and one consumer (the recorder) share a ring of 64-bit words. A packet is a header word
(op, length in words) and the call's arguments; its pointers point into the packet itself, so the recorder passes the
payload to the driver as it is. A packet never wraps: one that does not fit before the end is preceded by a pad. The
producer publishes head after each packet; the recorder makes the calls and advances tail, which the producer waits on
when the ring is full and in `rec_drain`. While a part is recorded the recorder spins on head; idle, it sleeps on an
event, and a sleep that ends with nothing new goes on without spinning first. The producer wakes a sleeping recorder
once: it takes the sleeping flag back itself, so the packets it makes while the recorder is still waking cost no further
system call. head, tail and the producer's own words sit on cache lines of their own: the producer writes head and its
counters per packet while the recorder reads head in its spin, and the recorder writes tail.

The ring is `RING_WORDS` words, 16 MiB: a part of a submission is about 1 to 2 MiB of packets in the town. The state
around it:

- `head`: words published by the producer; `tail`: words consumed by the recorder.
- `sleeping`: the recorder waits for `wake_ev`, an auto-reset event.
- `put_h`: the producer's count of words written (published up to `head`); `tail_seen`: `tail` as the producer last
  read it in `wait_space()`, which is enough because `tail` only grows.
- `rec_cb`: the part being recorded, set while the recorder is idle; `rec_live`: `rec_begin()` started the recorder for
  `rec_cb`.
- `rec_prio`: the submitting thread's priority, which the recorder takes.
- `part_h0`: `put_h` at `rec_begin()`, where the packets of the part being recorded start; `part_fence`: that part's
  value.
- `RS_`: the producer's statistics, logged and reset by `rec_stats_log()`; `full_ns_all` and `drain_ns_all` run since
  the start for `rec_wait_totals()` and are never reset.

### `rec_main()`

The consumer. After a sleep (`slept`) nothing new means still idle, so it does not spin again before the next sleep.
The sleep has a 2 ms timeout, which is what a missed wake costs at most. `tail` is published every 64 packets and at the
end of each batch.

`NX2SARIA_REC_DELAY_US` (debug): the recorder falls this far behind each batch it finds, as on a slow machine, so
objects the replay lets go of meet packets it has not made yet (`NX2SARIA_REC_CHECK`).

### `wake_if_sleeping()`

Wakes the recorder when it sleeps or is about to: whoever takes the flag back sets the event, once per sleep. Setting
the event for a recorder that has not reached its wait yet makes that wait return at once.

### `wait_space()`

The first test uses `tail_seen`: when there is room even by the older tail, the producer does not read `tail`, the
recorder's cache line.

### `put()`

Room for a packet of `bytes` of arguments; the caller fills it and calls `pub()`.

With `NX2SARIA_REC_CHECK` the room is zeroed: packers reserve room for optional arrays they may leave unwritten
(`BeginRendering`'s depth and stencil), and those words would still hold handles from the ring's previous lap.

### `rec_begin()`

The first call decides whether there is a recorder (`NX2SARIA_VK_RECORDER`) and starts its thread; the debug switches
(`NX2SARIA_REC_CHECK`, `NX2SARIA_REC_DELAY_US`) are read before the thread starts. `rec_cb` is written without a lock
because the recorder is idle then: `rec_end()` drained it.

Default off on AMD GPUs. With the recorder those drivers lost the device or hung the GPU at startup on RDNA 2, 3 and 4,
under AMD's Windows driver and under RADV (issues `#5`, `#40`, `#277` and `#283`: a black screen, or
`VK_ERROR_DEVICE_LOST` with a breadcrumb naming a part of a replay submission, and `vk_recorder` = 0 gets past it).
Keeping objects until their submission completed and ending a part from the recorder (`rec_end_cb()`) fixed the white
scene on an RX 6700 XT, but not this: 0.2.8 turned the recorder on for every GPU and the AMD reports came back the same
day. `NX2SARIA_VK_RECORDER=1` turns it on for an A/B run. Up to 0.10.1 the low-end GPU mode turned it on for AMD GPUs
without mesh shaders (GCN: RX 400/500, Vega; RDNA 1): every report above is from an RDNA 2-4 card, and those all have
mesh shaders. An RX 580 ran with it and `NX2SARIA_REC_CHECK` (no hazards), the replay's part of the game's wait after a
frame's first submission 6.9 ms instead of 12. The mode is gone (`runtime/cfg/cfg.c` `lowend_set()`): `vk_recorder` = 1
turns it on there as anywhere.

### `rec_end_cb()`

`NX2SARIA_REC_END` (default 1): the thread that recorded the part also ends it, after the breadcrumb crumb (kind 0:
none) that `runtime/vk/vk_sync.c` writes at the end of a submission. On an RX 6700 XT (driver 26.10.1, issue `#5`) parts
the recorder recorded and the submitting thread ended broke when a submission was split
(`runtime/vk/vk_sync.c` `vk_sync_split()`): the GPU hung or the scene turned green, then white. The handoff in
`rec_drain()` orders the two threads, so Vulkan allows the old split; the driver did not cope with it.
`NX2SARIA_REC_END=0` brings the old split back. The line is written only when a part was recorded by the recorder: with
the recorder off (the default on AMD) the switch changes nothing, and reports read an unconditional line as proof that
the recorder was running (`#315`).

### `rec_check_handle()`

`NX2SARIA_REC_CHECK=1` (debug, recorder on): the packets of the part being recorded are searched word by word for an
object `runtime/vk/vk_memory.c` lets go of. `put()` zeroes each packet's room, so only words a packer wrote can match. A
hit is a hazard.

- At a deferral (destroy 0) when the stamp tl is below the part's value: `runtime/vk/vk_memory.c` `collect_locked()` may
  destroy the object once the GPU reaches tl, before the part ran and, with the recorder behind, before it made the
  call;
- At a destruction (destroy 1) always: a packet not made yet hands the driver a dead handle, a made one leaves it in a
  command buffer not yet submitted.

"Made" means before the tail the recorder published, which trails it by up to 64 packets. Hazards are logged (the
first 64) and counted in `chk_hz`, indexed `[destroy][made][kind][op]`, for `rec_check_log()`; named deferrals that wait
long enough are only counted (`chk_named`). Reads race the producer only in words below head, which stay put until the
ring wraps (16 MiB later).

A part of more than half the ring is not searched, and the search stops at a header word that reads as length 0: the
producer overwrote it meanwhile.

### The packers

Each `rc_vkCmd*` wrapper makes the real call at once when the command buffer is not the part being recorded (`OWNED`),
and otherwise packs the call's arguments.

- **Fixed-size commands** (`FIXED`): the arguments after `cb`, as a struct.
- **Commands with arrays**: a struct with the call's arguments and the arrays after it, with pointers into the packet.
  `tail_copy()` copies the elements of an array after the fixed part at `*at` and returns where they went (NULL for
  none). The copies between buffers and images share one struct, `A_Xfer`, whose two handles are buffers or images.
- **A call the ring takes no packet for** (`FALLBACK`, a structure with a `pNext` chain): everything queued is made
  first (`rec_drain()`), then the call on the calling thread.
- **Extension commands** called through device function pointers have `rec_*` packers that carry the pointer.

### `rec_vertex_input()`

The binding and attribute descriptions are copied as they are, with their `pNext`: it is always NULL there, so this
packer has no fallback.

### `exec_packet()`

The recorder's side: one case per packet, which makes the call with the payload as its arguments. `R_EndCb` writes the
breadcrumb, when the packet has one, and ends the command buffer.

## `runtime/vk/vk_rec.h`

The recorder thread (`runtime/vk/vk_rec.c`): the Vulkan calls the replay makes into its command buffer are made by a
thread of their own. `runtime/vk/vk_sync.c` brackets each part of a submission with `rec_begin()` (after
`vkBeginCommandBuffer`) and `rec_end()` (before `vkEndCommandBuffer`). In between, every `vkCmd*` on that command buffer
copies its arguments into a packet of a ring, in call order, and the recorder makes the real call in that order; calls
into any other command buffer (uploads, the window, probes) are made at once. A call the ring has no packet for (a
structure with a `pNext` chain) waits for the recorder to empty the ring and is then made at once on the calling thread.
`rec_end()` returns once the recorder made every call. `runtime/vk/vk_sync.c` ends a part through `rec_end_cb()`: the
recorder makes the last calls into a part it recorded, `vkEndCommandBuffer` included; submitting and the command pool
stay on the submitting thread.

Every `vkCmd*` the runtime calls through `runtime/vk/vk.h` goes through the `rc_*` wrappers of `REC_CMDS` (the macros at
the header's end rename the calls, except in `runtime/vk/vk_rec.c` itself, which defines `VK_REC_IMPL`); the extension
commands called through function pointers have `rec_*` wrappers of their own. `NX2SARIA_VK_RECORDER=0` makes every call
at once (the same calls in the same order), `=ab` switches it with the A/B blocks (`rt_ab`), per submission part.

A recorder thread can make the Vulkan calls because the driver's work behind the commands depends on nothing the replay
decides next.

### `rec_begin()`, `rec_end()`, `rec_drain()`

`rec_begin()` is called by the submitting thread with the submission lock held; `fence` is the part's value.
`rec_end()` waits for the recorder, and does nothing when `rec_begin()` did not start it. `rec_drain()` waits until the
recorder made every call queued so far.

### `rec_end_cb()`

The end of part `cb`: the breadcrumb `crumb` (0: none, `runtime/vk/vk_crumbs.c`), then `vkEndCommandBuffer`. When the
recorder records `cb`, it makes both, and the function waits for it and returns 1; otherwise it does what `rec_end()`
does, writes the breadcrumb, and returns 0 for the caller to end `cb`.

### `rec_wait_totals()`, `rec_stats_log()`

`rec_wait_totals()`: the producer's time spent on a full ring and in drains. `rec_stats_log()`
(`NX2SARIA_TRACE=perf`): packets, bytes, drains and waits since the last call.

### `rec_check_handle()`, `rec_check_log()`

`NX2SARIA_REC_CHECK=1` (debug, recorder on): `runtime/vk/vk_memory.c` reports each object (`REC_IMAGE`, `REC_VIEW`,
`REC_BUFFER`) it defers (`destroy` 0, stamped `tl`) or destroys (`destroy` 1) with the value the GPU reached (`gpu`); a
packet of the part being recorded that still names it is logged as a hazard when the object can go before the part ran.
`rec_check_log()` writes the counts by kind and packet, at exit.

### The extension commands

Called through device function pointers, which the packet carries.

- `rec_push_tpl()`: `data` is the `bytes` bytes the template reads.
- `rec_bool()`: declared with depth clamp's type; alpha to coverage goes through it too.
- `rec_shading_rate()`: `size` x `size` pixels per fragment.
- `rec_pass_mark()`: a pass mark (`runtime/vk/vk_crumbs.c` `vk_pass_mark_cmd()`: a buffer marker and a checkpoint), in
  order with the commands around it.

## `runtime/vk/vk_sync.c`

Submission, fences and the completion thread of the Vulkan back end. One timeline semaphore (`VkCtx.timeline`) orders
everything: each submission records its command handles into a primary command buffer (`vk_exec_record`) and signals the
next timeline value, which is the NVN fence value the front end hands to syncs. The completion thread waits on the
timeline and, in submit order, runs the CPU side effects queued for each value (event writes, counter reports, sync
state), then frees the memory and texture objects whose users completed. A value counts as done only after its side
effects ran, so a guest woken by `fence_wait` sees them.

One timeline value per submission, signalled at `ALL_COMMANDS`; waits use `vkWaitSemaphores`, and a failed wait is
treated as a lost device.

- `CmdSlot`: reusable once the timeline reached fence.
- `sub_lock`: Submissions: one at a time, so timeline values are signalled in the order they are handed out.
- `cur_slot`: the slot `vk_sync_submit` records into now (an index: `take_slot` may move slots).
- `cur_fence`: the value of the submission being recorded.
- `PART_CBS`: The command buffers of the part being recorded that come before `slots[cur_slot]` (`vk_sync_append`):
  earlier pieces of the replay's own and those the replay workers recorded, in queue order.
- `st_lock`: Shared with the completion thread (`st_lock`).
- `cv_work`: a submission, a callback or stop.
- `cv_done`: done advanced.
- `issued`: newest submitted value; newest value whose side effects ran.
- `reserved`: asynchronous replay: newest value handed out before its submission (>= issued).
- `LIVE_RING`: Staged mirror uploads (`runtime/vk/vk_memory.c` "staged mirror uploads"): per submission value, whether
  its GPU still reads guest memory itself (value << 1 | live), under `st_lock`. A value not found counts as live.
- `live_last_of`: Per submission value: the newest submission up to it whose GPU reads guest memory itself (`live_of`),
  0 none. The submit wait is for the frame's first submission, but the game's writes after it race with every submission
  before it that still reads guest memory: those of the frame before (the instances of its grass and moss, read through
  the device address table) were often still on the GPU when the wait ended at the first one's hand-over, and the game
  wrote the next frame's instances under them (flicker and sliding foliage, RX 580, 2026-11-11). The wait goes on until
  that one is done.
- `res_of`: Per value (index & `LIVE_RING` - 1): the newest value up to it whose completion hands results to the CPU
  (`vk_exec_take_results`).
- `res_kinds`: per value: its `VK_RES_*` kinds.
- `early_of`: The early tail (`runtime/vk/vk_exec.c` "the early tail"), per value under `st_lock`: `early_of` holds the
  value when its part with the write-backs signalled the executed timeline before its tail; `empty_of` when the
  submission handed the GPU nothing and the CPU no results (a submission held at its first event wait: Ocarina's first
  of a frame in the field).
- `tail_cb`: `sub_lock`: the tail's command buffer between hold and release.
- `tail_early`: `sub_lock`: this submission's tail went behind its write-backs; the parts of it handed over before the
  last.
- `tail_ns`: about how long the GPU takes for a tail (a moving average).
- `tail_t0`: the first early return for `tail_val`, for `tail_ns`.
- `tail_n`: since the last `vk_sync_tail_stats`: early tails; waits they ended.
- `res_mask`: `NX2SARIA_SUBMIT_FINISH_KINDS`: the kinds the wait counts.
- `queue`: side effects in the order they were added.
- `rqueue`: The side effects the game's CPU reads (`vk_completion_add_result`: occlusion counts, counter reports, event
  values), in a queue of their own: the wait after a frame's first submission runs those that are due itself
  (`results_run`, "the executed timeline" below). One runner at a time, in the order they were added.
- `res_lock`: the runner; taken before `st_lock`.
- `fqueue`: `vk_completion_add_first`: run before the callbacks of their value (merge lists).

### Submit batching

Every game submission used to be its own `vkQueueSubmit2`, about 150 a frame, each a trip into the driver on the render
thread. Now up to `hold_max` of them are held and go to the queue together, as one `vkQueueSubmit2` with one
`VkSubmitInfo2` each, in the order they were made; a part of a long submission (`vk_sync_split`) goes at once, with what
is held before it, since it was split off to start the GPU early. Queue order is what orders them with everything else,
so held work goes to the queue before anything else does or is waited for:

- Every other submitter, and every idle wait, takes the queue lock through `vk_queue_lock`, which flushes first;
- A fence wait for a value still held flushes (`fence_wait_impl`), as do `vk_sync_finish` and shutdown;
- The completion thread flushes what has been held for `HOLD_NS`, so a guest that polls memory a held submission writes,
  instead of waiting on its fence, still sees it soon; it only tries the lock, never waits for it.

H is guarded by the queue lock. `NX2SARIA_SUBMIT_BATCH=0` (or 1) hands each to the queue at once; `=N` holds up to N.

- `CHAIN_MAX`: Command buffers of one submission: the mirror prologue, the parts cut before the part with their pre-pass
  command buffers (`CHAIN_MAX`), the part's pre-pass command buffer, the part (`vk_sync_pre`, `vk_sync_cut`,
  `chain_infos`).
- `H`:
  - `VkLatencySubmissionPresentIdNV lat[HOLD_CAP]`: the frame's number for the driver's low latency
    (`runtime/pacing/vk_lowlat.c`).
  - `uint64_t last`: the newest timeline value a held submission signals, 0 none.
  - `uint64_t batches, subs`: `vkQueueSubmit2` calls and the submissions they carried.
- `held_n`: H.n, for readers without the lock.
- `HOLD_PAIRS`: The GPU timeline (`vk_gtl_note`): the timestamp pairs of the held submissions, stamped with the time
  they really go to the queue (`ts_queue_ns`), as a held part waits here and not in the queue. `hold_pend`: closed by
  `ts_submitted` for the submission `queue_submit` takes next (`sub_lock`); `hold_pairs`: those of the held ones (queue
  lock).
- `ts_queue_ns`: per pair (`TS_PAIRS`): when its submission went to the queue.
- `held_t0`: when the oldest held submission was held (`kern_now_ns`).
- `queued_v`: the newest timeline value handed to the queue.

### The executed timeline

The wait after a frame's first submission (`runtime/nvn/nvn_api_core.c` `nvnQueueSubmitCommands`) is there so that the
GPU has run that submission's passes before the game rewrites what they read. With `NX2SARIA_WB_SPLIT=0` it is the
fence's wait. With 1, the default, it is a wait on `exec_tl`, after which the waiting thread itself runs the results the
game reads (`results_run`, the last paragraph but one below). `exec_tl` is a second timeline that reaches a submission's
value when the GPU has executed its own commands: the replay ends the part before the write-backs
(`runtime/vk/vk_exec.c`, `vk_sync_split_exec`; `runtime/vk/vk_memory.c` `vk_mem_mirror_end()`: what the shaders wrote
into VRAM mirrors goes back to guest memory), that part signals `exec_tl`, and the write-backs follow in a part of their
own, which signals the timeline as before. A submission without that split signals both at its end. Every other wait,
the fences the game asks for among them, is the fence's either way. That was the default for a day, on the thought that
the wait covered 3 to 4 ms of write-backs a frame that nothing needed before the game went on. It left out more than GPU
work. A value counts as done only when the completion thread has run its side effects and those of every value before
it, and among them is what the game's CPU reads next: the occlusion counts, counter reports and event values
(`runtime/vk/vk_exec_xfer.c` `oq_cb()`, `counter_cb()`, `event_cb()`), and the stamps that tell the texture checks which
pages the GPU wrote (`vk_gpu_written`). A frame's first submission is small and runs right behind the last frame's large
one, so its signal on `exec_tl` comes while the completion thread is still at that one's callbacks. One frame with the
wait on `exec_tl`:

- The GPU finishes the last frame's main submission; the completion thread wakes and starts on its callbacks, 800 to
  1,000 occlusion counts among them in Hyrule Field;
- The GPU finishes this frame's first submission and signals `exec_tl`: the wait returns and the game goes on;
- The game reads its occlusion counts (a 0 culls what the query tested) while some still hold the frame before's; half a
  millisecond later, more when the CPUs are busy, the callbacks are through.

Measured in Hyrule Field with the camera turning (RTX 5090, lazy write-back on, 2026-11-11; `NX2SARIA_TRACE=perf`,
"waits after a submit on the executed timeline"): 2,267 of 2,320 such waits returned before those side effects had run,
which were through 0.45 to 0.8 ms later on average and 12.6 ms at most; in that time 770 to 1,015 occlusion counts, 21
to 28 counter reports and 17 event values a frame were written into guest memory. The submission the game waits for
wrote nothing back in any window of those runs, so its own write-backs were never what the wait left out. A player saw a
patch of grass near the camera change from frame to frame there, and less of it with the fence's wait; no scripted run
showed such a frame with either wait, so that the late counts are what the player saw is not shown. The fence's wait
takes 0.65 to 0.7 ms more of a frame there: 85.5 against 86 and 92 frames a second in the field, 80.7 against 85.8, and
98 against 105 in a village, in single runs beside other work. Later a player's capture showed it plainly: in Hyrule
Field, the camera nearly still, trees, ground and walls lost their direct light for single frames about every 36 frames
(the game's own cycle there), 57 of 1,800 frames, with the wait on `exec_tl` alone and 28,000 to 33,000 occlusion counts
per 60 frames written after the game had gone on. The fence's wait covers them but waits for everything else the
completion thread does for the last frame's submissions as well: of its 0.49 ms a frame, 0.11 ms are the occlusion
counts and 0.23 to 0.40 ms one loop that stamps the pages those submissions' shaders wrote
(`runtime/vk/vk_exec_draw.c` `cb_done()`), which the game does not read. So the results the game reads have a queue of
their own (`vk_completion_add_result`: occlusion counts, counter reports, event values), and the waiter runs those of
its submission and of the ones before it as soon as `exec_tl` is reached (`results_run`; the GPU ran those submissions
first, so their queries are complete). The completion thread runs them the same way when it reaches a value, before its
other callbacks, and one runner at a time keeps them in the order they were queued. What stays behind the wait: the
write-backs and the page stamps, as with the bare wait. A player's hand test of this wait at the place of that capture
showed none of those frames (2026-11-11). No scripted run has measured it: the one run made for it, with the wait on
`exec_tl` alone, standing in the same field, had 8,147 of 8,160 waits back early and 1,170 occlusion counts a frame
written after them, and showed no such frame either, so the scripted scene does not reproduce what the player saw. What
the wait costs against the bare one is not measured; the results it runs take 0.11 ms a frame on the completion thread
there. `NX2SARIA_WB_SPLIT=3` is the wait on `exec_tl` alone, as it was, to measure against. With shadow storage
(`runtime/vk/vk_memory.c`) `NX2SARIA_WB_SPLIT=1` keeps the fence's wait as well: there the write-backs themselves reach
guest memory in a completion callback. With an import a write-back stores into guest memory on the GPU, right behind the
passes. With a shadow copy it stores into the copy, and the bytes reach guest memory in the fence's completion callback
(`shadow_wb_cb`), after the write-back part, the fence and the callbacks queued before it, while the game is already
rewriting for its next frame what those passes wrote. A write-back that lands after such a rewrite puts the GPU's older
bytes over it in guest memory and moves the page's base copy along, so the next copy-in finds nothing the CPU changed;
and a rewrite with the value the base copy holds (a reset to zero) is no difference at any time. The GPU then goes on
from its own last output, as the game's lighting accumulators do, which the shaders add to and the CPU resets. Measured:
with the split the whole 3D scene came out brighter for single frames (3.8 of 255, the interface as it was), each of
them a frame whose main work was held at its CPU event wait and replayed late, together with the next frame's: standing
outdoors, 695, 624, 443, 323 and 2 of 1,800 frames in five runs, and none in eight runs without the split (RTX 5090 with
`NX2SARIA_SHADOW=1`, 2026-11-10). A player on Linux (RX 7800 XT, RADV) saw the open field lit in one frame and flat in
the next; that it is the same fault is not confirmed on their machine. Which words the write-backs overtook was not
traced: `NX2SARIA_SHADOW_CHECK` counts no byte that both sides changed to different values, which fits a reset to the
base copy's value. `NX2SARIA_WB_SPLIT=2` keeps the split with shadow storage too, to measure that fault.

- `exec_sig`: `sub_lock`: the newest value a submitted part signals on `exec_tl`.

### A lost device in the timelines' values (`runtime/vk/vk_sync_hw.h`)

The newest value any signal of the timeline (`tl_hw`) and of `exec_tl` (`ex_hw`) carries, noted before the signal can
happen: by `hold_flush_locked` before `vkQueueSubmit2`, by the host's own signals before `vkSignalSemaphore`. A value
read above the note ends the run as a lost device (`timeline_beyond`). Reaching those values is no proof the device
lives: a driver that reports every pending signal as done after a reset reads at or below the note, and the line an
occlusion query left unwritten logs (`runtime/vk/vk_exec_xfer.c` `oq_cb()`, `vk_sync_state_text`) shows which of the two
a report had.

- `wb_results`: the wait on `exec_tl` also runs the results the game reads (not with 3).
- `exw_n`: What a wait on `exec_tl` leaves behind: the completion thread's side effects for the values up to the
  waiter's. Counted for `NX2SARIA_TRACE=perf` (`runtime/vk/vk_exec.c` logs them): the waits that returned with such side
  effects still to run, how long after the return they were through, and (`vk_sync_effect_late`, asked by the callbacks
  that write into guest memory) how many of those ran in that time.

  since the last `vk_sync_exec_stats`.
- `exw_value`: the newest wait that returned early: its value and when; 0 none.
- `cb_late`: the callback being run belongs to a value such a waiter is past.
- `cb_value`: The value whose side effect the calling thread is running, 0 when it is not the completion thread inside
  one.
- `CBT_N`: `NX2SARIA_TRACE=perf`: the completion thread's time by callback (`st_lock`), since the last
  `vk_sync_cb_stats`. A callback is named by its distance from `vk_completion_add`, which the build's link map turns
  into its function.

### `lost_at_due()`

`NX2SARIA_DEVICE_LOST_AT=n` (a test of the lost device's end, `runtime/vk/vk_device.c` `vk_device_lost_check()`): the
n-th hand-over to the queue is not made and counts as one the driver answered with a lost device. 0 or unset: never.

- `return at && ++calls == at`: queue lock held.

### `hold_flush_locked()`

- `if (vk_device_is_lost())`: the run is ending: the held submissions are dropped, nothing reaches the queue.
- Every value this hand-over signals is noted before the queue can reach it ("a lost device in the timelines' values").

### `vk_sync_flush_held()`

- `if (!H.n) return`: queue lock held: H is stable.

### `queue_submit()`

The replay's submission si (signalling value, 0 for a part without a signal), held or handed to the queue.

- The present holds this lock for its whole flip. A slow flip (frame generation allocating, a swapchain remade) makes
  the submitting thread wait here; counted as a present wait, so the slow-frame line no longer shows such a frame with
  nothing blocked.
- `if (si->signalSemaphoreInfoCount)`: at most two: the timeline and the executed timeline (`exec_tl`).
- NVIDIA's low latency on (`runtime/pacing/vk_lowlat.c`): the submission carries the number of the frame being replayed,
  so the driver counts its GPU time to that frame. The callers' submissions have no chain of their own.
- A part of a long submission (no value) goes at once with whatever is held: parts exist to start the GPU early. So do
  held submissions that have waited `HOLD_NS`, and a full batch.

### `hold_release()`

Hands the held submissions to the queue when value is among them (a waiter needs it) or they have waited `HOLD_NS`
(aged). The completion thread passes `try_only`: it never waits for the queue lock, whose holders may wait for it.

- `if (!vk_queue_trylock_raw()) return`: the holder's thread is submitting: it flushes or holds on.

### `vk_sync_release_held()`

The replay thread is about to wait for what submission `value` leaves (`runtime/vk/vk_memory.c` `apply_wait()`,
`NX2SARIA_MERGE_CPU`): a held submission that signals it goes to the queue now, or the wait would last out the hold.

### GPU time

(`NX2SARIA_TRACE=perf`): each submission part (`vk_sync_split`) writes a timestamp when it starts and when it ends, into
a ring of query pairs. The completion thread reads a pair once the part's value is reached and adds the part's interval
to the busy time, counting overlap with the previous part once. Off (one log line) when the queue family has no
timestamp bits or the period is 0. Upload batches (`runtime/vk/vk_memory.c`) are not timed.

- `ts_state`: 0 not set up, 1 on, -1 off.
- `ts_period`: ns per tick.
- `ts_mask`: valid bits of a timestamp.
- `ts_next`: submitting thread (`sub_lock`): next pair.
- `ts_busy`: written until the completion thread read the pair.
- `ts_cur`: the pair of the part being recorded, -1 untimed.
- `ts_last_end`: completion thread: end of the latest part read (ticks).
- `ts_busy_all`: The same durations, never reset: dynamic resolution differences it across presented frames (the perf
  window consumes `ts_busy_ns`, so a reader that sampled it would steal from the perf line and see a reset every
  window).
- `ts_sub_ns`: GPU idle (with calibrated timestamps): the gap before each timed part, split by whether the part was
  submitted after the previous one had ended (the GPU waited for the CPU: starved) or before (it waited for untimed work
  or a wait).

  CPU time the part was handed to `vkQueueSubmit2`.
- `TsRead`: A pair to read at its value. mirror: the copies of a mirror prologue (`runtime/vk/vk_sync.c` prologue),
  counted apart as well. mirror: `GTL_PART`, `GTL_MIRROR` or `GTL_PRE` (the GPU timeline's kinds, below); cb: the
  command buffer it times.
- `TS_UNSENT`: Closed and not yet submitted (`sub_lock`): with cuts a submission carries up to `SUB_CBS` command
  buffers, each timed; with room for 4 the rest went to the completion queue at once, without a submit time and out of
  the GPU's order, and their gaps counted as idle "with it queued".
- `ts_mirror_ns`: of `ts_busy_ns`, the mirror prologues' copies.
- `ts_last_end_ns`: completion thread: `ts_last_end` on the CPU clock.
- `cal_tick`: completion thread: a calibrated (GPU tick, CPU ns) pair and when it was taken; the GPU timeline's notes
  read them from other threads.

### `ts_setup()`

On the first submission that wants timing (`perf_gpu_timing`: `NX2SARIA_TRACE=perf` or the full performance overlay).

### `cal_refresh()`

Completion thread: refresh the GPU tick / CPU clock pair every 250 ms; 0 when there is no calibration. The host domain
is the clock `plat_now_ns` reads (QPC on Windows, `CLOCK_MONOTONIC` elsewhere), `plat_counter_hz` ticks a second.

### `tick_ns()`

- `if (cal_fn(c->device, 2, ci, v, &dev) != VK_SUCCESS)`:
  - `if (!cal_at)`: the domain pair is not supported: no split.

### The GPU timeline

(`NX2SARIA_TRACE=perf`, `runtime/vk/vk.h` `vk_gtl_note()`): every timed piece of GPU work, with the CPU time it was
handed to the queue and its GPU start and end on the CPU clock (calibrated timestamps). The replay's parts, their mirror
prologues and pre-pass command buffers come from here, the upload batches from `runtime/vk/vk_memory.c`, the presents
from `runtime/gfx/gfx_present.c`. Every 60 frames (`vk_gtl_log`) the pieces are put in GPU order and each gap between
them is split: starved, nothing handed to the queue yet, or queued, the next piece handed over and not started; the
queued part is summed by the kind of the piece that started after it. A gap with work queued is a wait inside the queue
(a semaphore, the present's acquire) or the driver's and the system's delay to start work on an idle GPU.

- `GtlEv`: call: handed to `vk_sync`'s submit; sub: to the queue.
- `PART_RING`: The last timed pieces, for the line of a frame whose render thread waited long for the GPU
  (`vk_gpu_parts_log`): each piece's hand-over to the queue, GPU start and end (`plat_now_ns`) and its kind
  (`runtime/vk/vk.h` `GTL_*`). Every kind enters, through `vk_gtl_note_ns` under `gtl_lock`: the replay's parts as the
  completion thread reads them, the upload batches when their batch retires (`runtime/vk/vk_memory.c` `up_ts_read()`),
  the presents when their slot is used again (`runtime/gfx/gfx_present.c` `pts_read()`). So the entries are not in GPU
  order; the reader sorts what it takes. It takes the newest `PART_RING` / 2 entries, which the writers do not reach
  before it has read them unless that many pieces end meanwhile (an area load adds hundreds of upload batches).

### The game's waits for GPU work

(`NX2SARIA_TRACE=perf`): for each submission value, when the game handed it to the replay (`vk_sync_reserve`; the submit
itself without asynchronous replay), when the replay had recorded and submitted it (issued) and when it was done (GPU
and completion). A fence wait of any thread but the replay's is split into its time until the waited submission was
submitted (the replay still recording it or those before it) and its time after (the GPU, the submit batching's hold,
the completion thread). Every 60 frames with the GPU timeline (`vk_gtl_log`).

- `gw_need_kind`: `VK_RES_KINDS`; `gw_rec`: `vk_sync_recorded_wait`, those it ended.

### `gw_at()`

`gw_lock` held.

### `gw_wait()`

The waits: t0 to t1 for value; newest: the newest value handed out at t0, sent: the newest submitted at t0.

### `vk_gtl_log()`

- `double held = 0, held_idle = 0`: replay pieces: held in the submit batching; of it, while the GPU was idle.
- `if (e->start > end)`: a gap before e.
- `if (e->sub >= end) lat[e->kind] += (double)(e->start - e->sub), nlat[e->kind]++`: handed to an idle GPU.

### `ts_submitted()`

Just before `vkQueueSubmit2` (`sub_lock` held): the parts closed since the last submit leave the CPU now. Their reads
are queued in the order the GPU runs them: a mirror prologue is closed after the part it was made for but runs first.
Read after its part, its copies counted neither as busy time nor as a gap of their own: they showed up as idle time with
work queued, 7-8 ms a frame over PCIe 3.0 in Kokiri Forest, and the frame as CPU-bound. cbi: the submission's command
buffers in queue order; their pairs are read in that order (a part's pre-pass command buffer and a cut part's are closed
in another order than they run).

- `for (uint32_t k = 0; k <= ncb; k++)`: k == ncb: whatever no command buffer named, last.

### `ts_read_cb()`

Completion thread, in submission order.

- `vk_gtl_note_ns(t->mirror, sub, q ? q : sub, sn, en)`: also the parts' ring (`vk_gpu_parts_log`).
- `} else plat_inc64(&ts_missed)`: the counter wrapped.

### `part_by_start()`

`NX2SARIA_TRACE=perf` (`runtime/diag/perf.c`): a frame whose render thread waited 20 ms or more for GPU fences gets, at
the next present, a line of the timed parts that ended in its interval [t0, t1]: how many, the GPU time they took, the
longest ones with their kind and the time they waited in the queue, and the longest gap between two. In a session of
2026-11-13 frames of 60-96 ms followed each area load by 2 to 6 frames, the render thread waiting 59-86 ms for the GPU,
and no line said which work the GPU did then. Read from the ring `vk_gtl_note_ns` fills, without its lock: nothing here
waits for the GPU or for another thread. "In the queue" is the time from a piece's hand-over to the queue to its GPU
start (for a replay part held by the submit batching, from the batch's hand-over, not from its submit call).

### `vk_gpu_parts_log()`

- `for (uint64_t i = from; i < n; i++)`: those that ended in the interval, then in GPU order: the gaps need it.

### Frame timeline

`NX2SARIA_GPU_TIMELINE=<frame>`: from that presented frame on, `TL_FRAMES` frames of what the CPU and the GPU did go to
the log once they are complete, in milliseconds from the first of those presents: each replay's begin and end and its
thread, each timed part's submit, GPU start and end (calibrated timestamps, so with `NX2SARIA_TRACE=perf`), the waits
for the GPU after a frame's first submission, the replay workers' groups, chunks and the submitting thread's waits for
them, and the presents. It tells whether the GPU waits for the replay, for a wait, or for the CPU behind one. Off (and
one load per note) without the setting.

### `gfx_tl_present()`

- `if (f == tl_from + TL_FRAMES + 3)`: the parts of the last frame have been read back by now.

### `ts_open()`

At the start of a command buffer (`sub_lock` held): the pair it times, -1 untimed.

- `ts_setup(vk_ctx())`: the overlay turned full: timestamps from now on.

### `ts_close()`

At its end, before `vkEndCommandBuffer`; fence: the value whose completion covers it. The read is queued at the submit
(`ts_submitted`).

- `else vk_completion_add(fence, ts_read_cb, t)`: more than a part and its prologue between two submits.

### `ts_begin()`

The pair of the part being recorded.

### `ts_drop()`

An empty submission records no command buffer: its pair is never written.

### `vk_exec_mesh_flush()`

Without `runtime/vk/vk_exec_draw.c`: nothing is queued.

### `vk_reg_replay_begin()`

Without `runtime/vk/vk_backend.c`: nothing registers through a back end, so no registration waits for a submission.

### `vk_exec_record()`

Weak stand-in for `runtime/vk/vk_exec.c`'s replay: only empty recordings ({`OP_BEGIN`, `OP_END`}) can be submitted.

### `vk_exec_held()`

Without `runtime/vk/vk_exec.c` (the probes): no submission is held back for an event.

### `timeline_beyond()`

Raw, read from the timeline whose signals hw notes: above every one of them the device is lost, and the run ends here
(`vk_device_lost_check`) after a line with the two values.

### `vk_sync_state_text()`

No lock: the thread that reports a lost device may hold any (`runtime/vk/vk_device.c` `device_fault_report()`).

### `vk_sync_progress()`

The hang watch's reading (`runtime/diag/hang_watch.c`): the newest value submitted and the newest done, without a lock
or a device call.

### `completion_main()`

- Held submissions that have waited long enough go to the queue; while some are held, the wait is short so their age is
  checked again (submit batching).
- `VkResult r = vkWaitSemaphores(c->device, &wi, tmo)`: 100 ms: stop is checked between waits.
- A lost device is reported here, where it is first seen. Unchecked, the value was taken as it came: the callbacks of
  submissions that never ran were called as if their work were done (one report's first sign was an occlusion query "not
  available after its fence"), and the game went on recording until the next checked queue call failed, an upload's,
  which then read as the cause.
- Both calls may succeed on a lost device and give a value its submissions never reached. A value above every one the
  timeline's signals carry ends the run ("a lost device in the timelines' values"); then, before a new value counts, the
  device is asked once more with a call of its own (`runtime/vk/vk_device.c` `vk_device_lost_poll()`, which ends the run
  when it is lost). That poll alone did not see the losses of the RX 6000 reports of 0.9.1 and 0.9.2: their callbacks
  ran for work the GPU never finished.
- Another thread saw the device lost (`vk_lost`): no value counts from here on and no callback runs. Theirs is work that
  never finished: frees of what the GPU may still hold, records handed out again, counter reports. done stays where it
  is, the waiters stay asleep, and the thread that saw the loss ends the process.
- The merge lists first (`NX2SARIA_MERGE_CPU`): the results the game reads come after the write-backs before them, as
  with the GPU's merge.
- `results_run(reached)`: what the game reads first; a waiter after its submit may have run them already.
- `if (ev && reached >= ev)`: the side effects a waiter on `exec_tl` went ahead of are through.
- The waiters go first (a fence wait, the frame's first submission, the acquire): freeing what the finished submissions
  held (`vk_mem_collect`: upload batches, destroyed images, views and buffers, each checked against the timeline itself)
  is nothing they read, and it takes `cmd_lock`, which the upload path holds, so done waited behind uploads. It only
  tries that lock (`vk_mem_collect_try`): an upload flush can hold it while it waits behind a present for the queue
  lock, and the next value's done waited too.
- `if (tv && reached >= tv)`: the tail a wait went ahead of is through: how long it took from there.

### `vk_sync_start()`

- `sync_hw_raise(&tl_hw, now > issued ? now : issued)`: before the restart's signal below.
- `sync_hw_raise(&ex_hw, now > issued ? now : issued)`: the executed timeline below starts there.
- `if (issued > now)`: a new device after `vk_shutdown`: values keep increasing for the front end.
- The executed timeline starts where the timeline is; without it every exec wait is a fence wait.
- `vk_astc_mode()`: decides `NX2SARIA_ASTC` and builds the ASTC decoder now, not at the first ASTC upload.

### `claim_slot()`

The index of a command buffer the GPU no longer uses, claimed for value `fence`. Called with `sub_lock` held; may move
`slots[]`.

### The part's pre-pass command buffer

(`runtime/vk/vk.h` `vk_sync_pre()`; `runtime/vk/vk_exec_draw.c` mesh emulation, `NX2SARIA_MESH_EMU_BATCH`) Begun at the
first call of a part, submitted right before the part (after the mirror prologue) and ended there. Its own pool: with
the recorder on, the recorder thread records the part (`cmd_pool`) while the replay records this.

### `vk_sync_pre()`

- `pre_slots[i].fence = cur_fence`: the submission's value: its parts are done by then.
- Everything earlier in queue order (the parts before, the mirror prologue) before what it records.

### `pre_end()`

- `chain`: Command buffers of the submission that a cut (`vk_sync_cut`) ended and that go to the queue with the next
  part, in order before it: a cut part's pre-pass command buffer, then the part.

The pre-pass command buffer of the part that goes now, ended; `VK_NULL_HANDLE` without one.

- `vk_exec_mesh_flush(cb)`: the stages of the pass's emulated draws, by stage.
- `vk_meshfb_pre_end(cb)`: the mesh fallback's batched draws (`runtime/vk/vk_mesh_fb.c`): their barrier, and their parts
  start over.

### `prologue()`

The copies the replay's first accesses to VRAM mirrors queued (`runtime/vk/vk_memory.c`), in a command buffer of their
own that is submitted right before the part that needs them and timed like a part; `VK_NULL_HANDLE` when there are none.
After the part was recorded, `sub_lock` held; may move `slots[]`. First brings the shadow storage up to date with the
CPU's writes, which needs no command buffer.

- `vk_mem_shadow_sync(fence)`: shadow storage: the CPU's writes go in before the part is submitted.
- `vk_mem_lazy_part_end()`: what the part left in the mirror and has to write back after all joins the snapshots.
- `int i = claim_slot(c, fence)`: before reading slots: the claim may move it.

### `chain_infos()`

What goes to the queue for a part: the mirror prologue, the parts cut before it with their pre-pass command buffers and
pieces (the chain, emptied), the part's own pre-pass command buffer, the part's earlier pieces (`vk_sync_append`: the
replay's and its workers', emptied), the part's last command buffer. Any of them may be missing.

### `vk_sync_cut()`

A cut: the part ends and the replay goes on in a new command buffer, as at a split, but nothing goes to the queue: the
part (its pre-pass command buffer, its pieces and itself) waits in the chain for the next part that does. The texture
pool set stays as it is, as nothing of the submission has been handed to the GPU. `VK_NULL_HANDLE` when the chain is
full.

- `if (!part_cbs[i]) return VK_NULL_HANDLE`: a replay worker's piece not filled yet: the part goes on.
- `CmdSlot *s = take_slot(c, cur_fence)`: the cut part keeps its slot until the value is reached.

### `vk_sync_append_room()`

The replay workers (`runtime/vk/vk_exec.c` "replay workers"): the replay's command buffer being recorded ends and joins
the part's list, then n places for the command buffers the workers record, in that order, and a new command buffer of
the same part is begun and returned: the same timeline value, timed and submitted with the rest, so the workers'
commands run between the replay's commands before and after. The places are filled (`vk_sync_append_fill`) before the
part is submitted, which `chain_infos` checks. `vk_sync_append_room`: whether the list has room for n more.

### `vk_sync_append_later()`

- `if (!rec_end_cb(cb, 0, 0)) vkEndCommandBuffer(cb)`: every command of it is in cb, ended by its recorder.
- `CmdSlot *s = take_slot(c, cur_fence)`: the ended one keeps its slot until the value is reached.

### `vk_sync_worker_begin()`

- `SYNC_WORKERS`: the replay workers (at most 16) and the submitting thread.

The replay workers' command buffers: each worker has a command pool of its own (a pool belongs to one thread at a time)
and claims command buffers from it for the submission being recorded, as the replay claims its slots. Called on the
worker while the submitting thread holds `sub_lock` and records on (`NX2SARIA_REPLAY_PIPE`) or waits for it: the part
with the worker's command buffer is not submitted before it is done, so `cur_fence` stays put.

### The early tail

(`runtime/vk/vk_exec.c` "the early tail"). The replay cut the part where the tail began (`vk_sync_cut`), so the chain
holds what came before and the command buffer being recorded is the tail's. `vk_sync_tail_hold` sets that one aside,
ended and timed as a part of its own, and begins the one the write-backs are recorded into; `vk_sync_tail_release` hands
the chain and the write-backs to the queue with the executed timeline's signal (`split_part`), and puts the tail into
the chain for the submission's last part, which signals the timeline. The queue runs them in that order, so the GPU's
work is what it was with the write-backs moved before the tail.

### `vk_sync_tail_hold()`

- `CmdSlot *s = take_slot(c, cur_fence)`: the tail keeps its slot until the value is reached.

### `wait_target()`

The newest value up to `value` whose GPU work a wait of the game is for: the empty submissions after it are passed over.
`*early`: that one's write-backs went before its tail, so the executed timeline reaches it there.

### `vk_sync_split_exec()`

The part that ends here is the last with the submission's own commands: it signals the executed timeline.

### `split_part()`

- `if (!rec_end_cb(cb, 0, 0)) vkEndCommandBuffer(cb)`: every command of the part is in cb, ended by its recorder.
- `vk_upload_flush(0)`: the uploads this part asked for precede it in queue order.
- `uint32_t ncb = chain_infos(cbi, pro, pre, cb)`: the mirror copies, the cut parts, the pre-pass work, the part.
- `queue_submit(c, &si, 0)`: held or handed to the queue (submit batching).
- `vk_mem_note_submit(cur_fence)`: objects destroyed from here on may be in use by the part just submitted.
- `CmdSlot *s = take_slot(c, cur_fence)`: the submitted part keeps its slot until the value is reached.

### `vk_sync_reserve()`

Asynchronous replay (`runtime/vk/vk_backend.c`): the value the next queued submission or present will signal. Values are
handed out in queue order and the replay thread submits them in that order, so each `vk_sync_submit` gets exactly its
value.

### `submit()`

own: NULL for the replay of handles; else the caller's commands in the replay's place (`vk_sync_submit_own`).

- The run is ending (`runtime/vk/vk_device.c` `vk_device_lost_check()`): nothing is recorded or submitted and no value
  is issued. The value returned is the last one issued; a wait for it sleeps until the process ends.
- `vk_present_frames_owed()`: frames presented while another thread replayed close before this replay begins.
- `if (n > 0 && !own) vk_sub_in_frame_publish()`: this submission's place in its frame, for the replay.
- `vk_mem_dirty_submit()`: `NX2SARIA_TRACE=perf`: what a mirror would upload before this submit.
- `NX2SARIA_REPLAY_TIME`: the replaying thread's CPU time over the replay too (`perf_replay_cpu`), against its wall
  time.
- `vk_gpu_passes_fence(fence)`: the submission the parts and results that follow belong to.
- The game's registrations made from here to the queue call below take effect after it (`runtime/vk/vk_backend.c`
  "registrations in submit order"): the replay writes the submission's texture pool set as it goes.
- `s = &slots[cur_slot]`: the last part (`vk_sync_split`).
- The recorder is done with the part: the end-of-submission crumb, then the end, by the recorder when it records.
- After the recording: the uploads it asked for (and every earlier one) precede it in queue order.
- `int sent = recorded || nchain || npart_cbs`: a cut part or a piece goes with the part after it, which goes too.
- The timeline, and the executed timeline unless a part of this submission signalled it already (`split_part`).
- `queue_submit(c, &si, fence)`: held or handed to the queue (submit batching).
- After the last part's prologue: every staged copy of the submission is taken; a probe's submit leaves the note to the
  replay's submission it may have come in the middle of.
- `plat_cond_broadcast(&cv_done)`: `vk_sync_recorded_wait`.
- `if (subs % (4096 * 8) == 4096)`: statistics only: read without the queue lock.

### `vk_sync_submit_own()`

The upload batch is no place for a probe's commands: it is submitted before the memory prologue.

### `vk_present_frames_owed()`

`runtime/vk/vk_backend.c`'s, which run the frames' bookkeeping (`runtime/vk/vk_frame_owed.h`) and set the place in the
frame of the submission being recorded; builds without the back end have neither.

### `pending_push()`

`st_lock` held: p at the end of the queue `*q`.

### `vk_completion_add_first()`

As `vk_completion_add`, in a queue the completion thread runs first: the merge lists the CPU stores into guest memory
(`NX2SARIA_MERGE_CPU`), which the results and the other callbacks of the same value come after. From the replay thread
alone, in value order.

### `vk_completion_add()`

- The completion thread sleeps only while nothing is issued beyond done: a callback for a later value (the replay's, for
  the submission it records: counter reports, freed resources) becomes due through the submit, which wakes it.

### `vk_completion_add_result()`

A side effect the game's CPU reads once the GPU has run submission fence's own commands: it runs before the other side
effects of the values reached, and a wait after a submit runs it itself (`vk_sync_exec_wait`).

### `results_run()`

Runs the results queued for the values up to upto, which the GPU has passed: on the completion thread for each value it
reaches, and on a guest thread whose wait on `exec_tl` has just returned. `st_lock` not held.

### `vk_sync_result_mask()`

The game's wait after a frame's first submission (`runtime/nvn/nvn_api_core.c` `NX2SARIA_SUBMIT_FINISH`): until the
replay has submitted value, which took every copy of guest memory the submission reads; then also until the GPU has run
it when its GPU reads guest memory itself (`vk_mem_take_live`) or `NX2SARIA_SUBMIT_FINISH_GPU=1`. 1 when reached.

### `vk_sync_recorded_wait()`

- `const char *km = cfg_get("NX2SARIA_SUBMIT_FINISH_KINDS")`: a number (`rt_knob` reads only 0, 1 and ab).
- `gpu = rt_knob("SUBMIT_FINISH_GPU", "1") != 0`: the low-end GPU mode set 0 up to 0.10.1.
- `if (value > issued && value > reserved) ok = 0`: never handed over.
- `uint64_t need = 0`: the newest submission up to value whose completion hands the CPU results.
- `uint64_t lneed = known ? live_last_of[value & (LIVE_RING - 1)] : value`: the newest reading guest memory.
- `if (!live) gw_wait(value, w0, w1, newest > sent ? newest : sent, sent)`: a GPU wait counts there.
- `if (need < value)`: what the submissions between need and value had that the mask leaves out.

### `fence_wait_impl()`

- `hold_release(value, 0, 0)`: a held submission the waiter needs goes to the queue now.
- `ok = 0`: never submitted nor queued: never reached (as in the null back end).

### `vk_sync_exec_wait()`

The wait after a submit: the fence's (`vk_sync_fence_wait`), or with `NX2SARIA_WB_SPLIT` until the GPU has executed
submission value's own commands (the executed timeline) and the results the game reads are in its memory
(`results_run`), its write-backs and the completion thread's other side effects for it and for the submissions before it
still to come. 1 when reached.

- The fence's wait unless the split is asked for ("the executed timeline" above says what the game reads that a wait on
  `exec_tl` leaves out). With shadow storage (`vk_mem_shadow_storage`) what the GPU wrote reaches guest memory in such a
  callback too: a wait that returned before it let the game rewrite a pool that an older write-back then overwrote
  (Kokiri Forest: a sky coefficient the game had reset came back, and the world flickered washed out). The run is ending
  (`runtime/vk/vk_device.c` `vk_device_lost_check()`): no wait on the lost device, and none for a value that will not be
  reached.
- The early tail: a value whose GPU work is that of an earlier submission with its write-backs before its tail (the
  submissions since handed the GPU nothing) is reached where that one's executed signal is, its tail still to run.
- `hold_release(asked, 0, 0)`: a held submission the waiter needs goes to the queue now.
- `if (r == VK_SUCCESS)`: a lost device may end the wait too: the results below would be read from work never run.
- The results the game reads, of this submission and of those before it, which the GPU ran first: here, not when the
  completion thread gets to them behind the other side effects of the last frame's submissions. The merge lists the CPU
  stores (`NX2SARIA_MERGE_CPU`) up to the value asked for, as the GPU's merges are by now.
- `uint64_t none = 0`: the first wait back for this tail starts its clock (the completion thread ends it).

### `vk_sync_game_wait()`

The game's own wait for a fence (`runtime/nvn/nvn_api_core.c` `nvnSyncWait`): the fence's wait, or with the early tail
the wait for the results of the submission the value's GPU work belongs to (`vk_sync_exec_wait`), when that one's tail
is all the GPU still has of it: the tail reads nothing the game can change and hands it nothing.

### `vk_sync_shutdown()`

- `qhead = qn = rqhead = rqn = fqhead = fqn = 0`: callbacks of values never submitted.
- The pre-pass command buffers' pool (`vk_sync_pre`: the mesh emulation's and the mesh fallback's batches); it stayed
  past the device's end, a leak each time the device was made again (the probes, a device made anew).

### --probe-devlost

(`runtime/host.h`): the end of a run whose device is lost, without a lost device and without a window. The device itself
stays healthy, so what the driver answers after a real loss is not covered here.

- A callback queued for the next value runs when the timeline reaches it (the check that step 2 can fail).
- With `vk_lost` set, as after another thread saw the loss: a callback queued for the next value does not run although
  the timeline reaches it, done stays, and a submit, an upload flush and a wait for a submission's commands return at
  once without issuing a value.
- With `vk_lost` clear again, four threads report a lost device at the same moment: the process must end with
  `RT_LOST_EXIT`, one report and one FATAL line in the output (`tests/device_lost_check.py` counts them). Reaching the
  end of this function is the failure.

### `probe_lost_advance()`

Makes the timeline reach the next value as a finished submission would: issued moves on, the semaphore is signalled from
the host, and the completion thread is woken. Returns the value.

- `sync_hw_raise(&tl_hw, v)`: a host signal: noted before it ("a lost device in the timelines' values").

### `vk_sync_probe_devlost()`

- `plat_sleep_ms(500)`: five of the completion thread's 100 ms waits.

### `vk_sync_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): command buffer slots (read without `sub_lock`, which a whole
replay holds), the completion queue and the submissions held back.

## `runtime/vk/vk_sync_hw.h`

The newest value each of the replay's timelines may reach, and the test a value read from one of them must pass
(the section "A lost device in the timelines' values" of `runtime/vk/vk_sync.c`). Header only, so that
`tests/sync_hw_check.c` checks it without a device.

A timeline semaphore changes only through signal operations, and each of them carries a value the program chose: for the
replay's two timelines, the queue hand-overs of its submissions (`runtime/vk/vk_sync.c` `hold_flush_locked()`) and the
host's own signals (the restart in `runtime/vk/vk_sync.c` `vk_sync_start()`, the lost-device probe's in
`probe_lost_advance()`, and the one of `runtime/vk/vk_device.c` `vk_selftest()`, noted through `vk_sync_signal_note()`).
A live device never reports a value above the newest of them. Each such value
is noted here before its signal can happen, so a value read above the note comes from a device that answers for work it
never ran: after a timeout reset, Windows releases every waiter of a timeline, and the reports of AMD's RX 6000 cards
(0.9.1, 0.9.2) ran the completion callbacks of submissions the GPU had not finished ("occlusion query slot ... not
available after its fence") before any call returned `VK_ERROR_DEVICE_LOST`.

### `sync_hw_raise()`, `sync_hw_beyond()`

`sync_hw_raise()` raises the note `*hw` to `v` and never lowers it; any thread may call it. `sync_hw_beyond()` is 1 when
`raw`, read from a timeline whose signals were all noted in `hw`, is above every one of them. The caller reads `hw`
after `raw`, so that a signal between the two reads cannot make a live device look lost.

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

### `pass_hooks_due()`

`in_chunk`: the calling thread records a chunk of a parallel group; `idx`: the chunk's place in its group; `ran`: the
group's hooks ran already (its first chunk began a pass). Outside a chunk every pass begin runs the hooks.

## `runtime/vk/vk_index_rewrite.h`

Index lists for the NVN primitives Vulkan has no topology for, made from an indexed draw's own indices
(`runtime/vk/vk_exec_draw.c` `draw_idx_rewritten()`, `NX2SARIA_QUAD_INDEX`). Header only, so that
`tests/index_rewrite_check.c` checks it without a device.

The replay draws a quad list as a triangle list and a line loop as a line strip
(`runtime/vk/vk_pipeline.c` `vk_pipeline_topology()`). For a draw without indices it makes the indices itself
(`op_draw`); an indexed one went to the GPU with the game's indices as they are: a quad list read three indices at a
time, which joins corners of neighbouring quads into triangles across the mesh, and a line loop without its closing
line. These turn the game's indices into the list the Vulkan topology wants: two triangles (0, 1, 2) and (0, 2, 3) per
quad, as the non-indexed rewrite makes them, and the loop's first index again at its end. Indices are read at their size
(1, 2 or 4 bytes, little-endian) and written as 32-bit ones; a base vertex stays the draw's.

### `idx_quads()`, `idx_loop()`

`idx_quads()` writes the indices of a quad list as triangles: `out` holds 6 * (`count` / 4), and a trailing partial quad
is left out, as NVN draws no incomplete primitive. `idx_loop()` writes the indices of a line loop as a strip that comes
back to its first vertex: `out` holds `count` + 1, and the caller passes `count` >= 2. Both return how many indices
they wrote.

## `runtime/vk/vk_oq_hold.h`

The count a samples-passed report writes when the replay could not count all of its draws
(`runtime/vk/vk_exec_xfer.c` `oq_report_count()`, `NX2SARIA_OQ_HOLD`). Header only, so that `tests/oq_hold_check.c`
checks it without a device.

A draw that finds every slot of the occlusion query ring still waiting for the completion thread is left uncounted, and
the report after it says only what the other draws counted, at least 1. The game reads these counts as how much of an
object shows, and a count of 1 where an object's proxy covers thousands of samples tells it the object is all but hidden
for that frame: whatever it scales by that count (a light's glare, an object's fade) drops for one frame and comes back,
a flicker. The ring fills while the GPU or the completion thread falls behind the replay, in a hitch at a few hundred
frames a second. With the hold such a report writes the larger of what was counted and what the last complete report
into the same record said: the previous frame's answer for that object. A record with no complete report remembered
keeps the old rule.

`OQH_N` records are remembered, direct-mapped by address (`oqh_slot()`): a collision forgets the older one. The slot
hash drops the address's low four bits, as records are 16 bytes apart at least.

### `oqh_note()`, `oqh_lost()`

`oqh_note()` remembers a complete report of `count` into the record at `addr`. `oqh_lost()` gives the count to report
into `addr` for a report that missed draws, of which the others counted `counted`. Neither remembers nor looks up a
record at address 0.

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

`TfrTarget` is one render target of a pass: the image handle as a number, the subresource, and the image's owner (0:
unknown).

### `tfr_find()`, `tfr_note()`

`tfr_find()` returns the target's entry. With `make` it returns the entry the target takes when it has none (the set's
oldest, an empty one first); without `make`, NULL. `tfr_note()` records that the target is rendered in frame `stamp`.
When the entry's owner is not the target's, another texture has the image now and the count starts again for it.

### `tfr_before()`, `tfr_first()`, `tfr_steady()`

`tfr_before()` is the frame before `stamp` that last rendered the target, 0 for none; `tfr_first()` says that no frame
before `stamp` did. `tfr_steady()` says that `TFR_STEADY` frames or more before `stamp` rendered the target under the
owner it has now.

### `tfr_pass_recent()`

Whether the pass with the `n_color` colour targets `c` and the depth target `d` (NULL: none) is one the game renders
frame after frame, so that a draw into it may be skipped in frame `stamp` and comes back in the next.

- `any` 0: every target is recent (and the pass has one).
- `any` 1: a colour target is, or the depth target is while no colour target is rendered for the first time. The game's
  item icon capture draws into a new colour target with the per-frame depth buffer, once, and its draws skipped there
  stay missing from the icon until the item is captured again. A colour target never rendered is neither recent nor
  steady.
- `steady` 1: a steady target counts as a recent one; `*by_steady` is set when only that made the answer 1. The
  function works both answers out side by side (`r[0]` without the steady rule, `r[1]` with it).

## `runtime/vk/vk_frame_owed.h`

A frame's bookkeeping at its present (`runtime/vk/vk_backend.c` `frame_end()`: the replay's per-frame state that a
present closes), in replay order. Header only, so that `tests/frame_owed_check.c` checks it without a device.

The replay reads and writes that state as it records, under the submission lock (`runtime/vk/vk_sync.c` `sub_lock`).
With the replay on the submitting threads the game presents on one thread while another may be replaying, and running
the bookkeeping then reset the state in the middle of that replay. The presenting thread does not wait for the replay:
it marks its frame owed and runs the bookkeeping itself only when it gets the lock at once; otherwise the next thread to
take the lock (`runtime/vk/vk_sync.c` `submit()`) runs every owed frame's bookkeeping, in frame order, before its own
replay. A frame caught up behind a later present runs as two presents with no submission between them always did.

The present thread's blit of a frame reads what that frame's bookkeeping closed
(`runtime/vk/vk_dlaa.c` `vk_dlaa_frame()`: the inputs frame generation must not get across an upscaler or frame
generation switch), so the bookkeeping must come before it, as it always did when it ran at the present. A frame left
owed may still be owed when the present thread takes it: `fo_close` runs what is owed then, under the lock, waiting for
a replay in progress as the present thread's own blit submission does a moment later. The present thread is not one of
the game's threads.

`FrameOwed` holds two frame numbers. `owed` is the newest presented frame: the presenting thread raises it and the
lock's holder reads it, so it is atomic. `done` is the newest frame whose bookkeeping ran: the lock's holder writes it,
and it is atomic for `fo_ran()`.

### `fo_waiting()`, `fo_run()`

For the lock's holder. `fo_waiting()` says whether a frame waits for its bookkeeping; it is what every submission
pays, two atomic loads. `fo_run()` runs the bookkeeping of every frame presented since the last run, in frame order,
and returns how many. `done` moves past a frame only once its bookkeeping returned, because `fo_ran()` reads it without
the lock.

### `fo_ran()`

Any thread: whether the frame's bookkeeping ran. When it did, what it wrote is visible to the caller (the release of
`done` in `fo_run()`).

### `fo_present()`

For the presenting thread, with frame numbers rising: a smaller one than `owed` leaves `owed` as it is, as that frame's
bookkeeping ran or is owed already. `try_run(arg)` takes the lock without waiting, runs `fo_run()` under it and returns
1, or returns 0 when another thread holds it. Returns whether the bookkeeping ran here.

### `fo_close()`

For the present thread, before it uses a presented frame (its blit): when the frame's bookkeeping has not run,
`run_locked(arg)` takes the lock, waiting for it, and runs `fo_run()` under it. Returns whether it had to; afterwards
`fo_ran(frame)` holds.

## `runtime/vk/vk_evo_peek.h`

A copy of the first held submission's wait of each event-ordered queue (the section "Event order across the game's
queues" of `runtime/vk/vk_exec.c`), for the readers that do not hold the submission lock. Header only, so that
`tests/evo_peek_check.c` checks it without a device.

`runtime/vk/vk_backend.c` `held_resume()` and `submit_sync()` ask `runtime/vk/vk_exec.c` `vk_exec_held_ready()` whether
held work can go on, without the submission lock. Following the queue's first continuation (the `Cont` of
`runtime/vk/vk_exec.c`) there read memory another submitting thread may free meanwhile (`runtime/vk/vk_exec.c`
`evo_resume()`, `cont_free()`): a continuation's wait lies in its private copy of the recording, and an event's address
read from a reused block points anywhere. So the holder of the lock publishes what the decision needs (whether work is
held, the wait it stopped at, by value, and when it stopped) each time a queue's first continuation or its stop changes,
and the readers take a copy from one publish (a sequence lock). A reader never waits for the writer: after
`EVO_PEEK_TRIES` attempts that overlap a publish it answers "not ready", which is what losing the race to the writer
answered before.

`EvoWait` has the layout of `p_wait_event` (`runtime/nvn/nvn_api.h`; a static assertion in `runtime/vk/vk_exec.c`
checks that the two agree), so it is passed to the event test as its payload. `EvoPeekCopy` is what one publish says:
`has`, a continuation is held; `has_at`, it stopped at a wait (else it has not started and goes on at once); `w`, that
wait; `since`, `vk_exec_submit_seq` when it stopped. `EvoPeek` is the shared copy; its `seq` is odd while a publish
runs.

### `evo_peek_publish()`, `evo_peek_read()`

`evo_peek_publish()` is for the writer, one at a time (`runtime/vk/vk_exec.c` `peek_publish()` calls it under the
submission lock). Its release fence after the odd sequence makes a reader that sees any field written after it also see
the odd sequence. `evo_peek_read()` is for any thread: 1 with `*out` a copy of one publish, 0 when every try overlapped
a publish. Its acquire fence makes the fields be read before the sequence is read again.

### `evo_peek_ready()`

The decision `runtime/vk/vk_exec.c` `cont_ready()` makes for a continuation, from a copy: it can go on when it has not
started, its wait holds (`met`, which `vk_exec_held_ready()` gives as `vk_exec_event_met_now()`), or it has waited more
than `max` submissions.

## `runtime/vk/vk_usnap.c`

Uniform blocks as of the submit call. NVN starts the GPU when `nvnQueueSubmitCommands` returns and the GPU reads the
constants soon after; here the replay records for several milliseconds, and the GPU reads a pool's bytes when it runs
the part that uses them (the sync copies of the VRAM mirror of `runtime/vk/vk_memory.c` run then). Meanwhile the
game, having handed the frame over, rewrites per-object uniform blocks on other threads with no fence: late draws read
another object's matrix (phantom rocks, a flat far field; the 1.2.1 game's producer `f_70008c6460`, 14-24 ms after the
submit). So at the start of a submission's recording every uniform range it binds is copied into an arena of its own,
and the uniform bindings of the replay use the copy.

- Only ranges of imported pools (memory the CPU writes) are copied, aligned out to 256 bytes (the largest
  `minUniformBufferOffsetAlignment`) and at most 64 KiB from the bound address, overlapping ones merged.
- A range the GPU wrote in a submission that has not completed is left out: its bytes reach guest memory only with that
  submission's write-back, the mirror holds them until then.
- A GPU write within the submission (UpdateUniformBuffer, a storage buffer a shader writes, a buffer copy or fill, an
  image-to-buffer copy: `vk_usnap_drop()`) drops the copies it touches from then on, so later draws read the result.
- The arena goes back to a free list once the submission's fence is done (completion thread).

The copy is taken at the submit call: where `vk_exec_record` starts when the replay runs on the submitting thread, and
by the submitting thread itself with `NX2SARIA_REPLAY_ASYNC` (`vk_usnap_take()`), where the record starts up to a frame
later on the replay thread. Taken only at the record's start, it held whatever the game had put into those bytes by
then: in a graveyard the block of the shadow pass came out with another block's values in the frame its allocator moved
on to the next stretch of memory, about every 364 frames, and that frame's sunlight was all in shadow (a dark frame with
the interface intact). `NX2SARIA_UBO_SNAPSHOT=0` binds live memory as before; `=ab` alternates (`NX2SARIA_AB`).

The structures:

- `Arena`: one submission's copies. `map` is the buffer's mapping. `cpu` is there with DLSS (`vk_dlaa_available`) or
  frame generation alone (`vk_dlaa_fg_only`): the same bytes in host memory for the CPU's reads (`vk_usnap_bytes()`),
  because `map` is often device memory through the BAR, written fast but read back at about a microsecond a cache line.
  Without either nothing reads them back, and the bytes go into `map` in one copy.
- `Reg`: a copied region. `dropped` is 0, 1 (a GPU write in flight: dropped for every lookup), or the earliest place in
  the stream (`vk_exec_stream_pos`) of a drop that ran.
- `Wrote`: a GPU write; `pos` is the write's place in the stream.
- `U`: the replay's state. `mode` is the `rt_knob` value stored plus 1 (0: not read yet; then 1 off, 2 on, 3 ab); `on`
  says this submission has a snapshot; `col` holds the collected ranges, `reg` the merged ones (sorted by `lo`,
  disjoint), `wr` the GPU writes of submissions not known to be complete. The counters from `subs` to `off_subs` run
  since the last summary; `chunk_drops` runs since the start: GPU writes recorded by replay chunks that dropped copied
  ranges.
- `gather_lock` guards `U.col`, the buffer of `sort_col()` and the neighbour cache of `collect()`: the submitting
  thread (`vk_usnap_take()`) and the replay thread (`vk_usnap_begin()`, for a submission nothing was taken for) both
  collect.
- `Taken`: a copy taken at the submit call for the replay thread (`vk_usnap_take()`), queued in submission order.
  `first` and `n` are the submission's first handle and handle count, by which `adopt()` knows it.

### `arena_get()`

The buffer is a transfer source too: DLAA copies the camera block out (`runtime/vk/vk_dlaa.c` `mv_pass()`). Its memory
is device-local through the BAR when the device has it (the GPU reads constants on every draw), else host memory.

### `arena_done()`

Completion thread: the GPU has read it.

### `collect()`

Walks a recording, through its jumps and calls (to `CALL_DEPTH`), and lists every `OP_BIND_UBO` range.

For the vertex stage's object and bone blocks it also lists the game's other copy on either side (`vk_mvprev_step`),
which still holds last frame's (`runtime/vk/vk_mvprev.c`). A neighbour is listed only when it lies in an imported pool:
one past its pool would make the whole merged region fail the pool check of the caller, and the bindings in it would
read live memory again. Which neighbours do is cached per block and `vk_mem_gen` (1024 entries): the same blocks are
bound by many draws, and two resolves for each binding were a quarter of the snapshot's time.

### `sort_col()`

Sorts the collected ranges by lo. A town frame binds about 10,000 uniform ranges and qsort took half a millisecond of
the copy's 0.96 ms on the game thread, so they go through an LSD radix sort on (lo - lowest) / ALIGN, three passes of 11
bits; qsort is left for a span that does not fit in 33 bits (2 TiB). Three passes end in the temporary buffer, so the
result is copied back.

### `wr_prune()`

Keeps the GPU writes of submissions that are not done, merged into disjoint ranges sorted by lo (the terrain rewrites
one block before every draw: hundreds of equal entries), each with the newest fence among its parts.

### `wr_hits()`

A binary search: it needs the list as `wr_prune()` leaves it, disjoint and sorted.

### `summary()`

Every 3600 submissions. The line ends with `vk_exec_clear_stats`: among the GPU writes, the small buffer clears and the
counter records they covered.

### `copy_some()`, `copy_down()`

The work of `copy_regions()`, sliced for the replay workers (`vk_exec_par`): `copy_some()` copies regions of the game's
memory into the arena, `copy_down()` the host copy into the device copy in `COPY_SLICE` pieces.

### `copy_regions()`

The bytes of regions `reg[0..nreg)` (`total` bytes in all) into the arena's device copy, through its host copy with
DLSS or frame generation, whose motion pass reads the camera (`runtime/vk/vk_dlaa.c` `mv_pass()`); the device copy is
then filled with sequential writes. With `par` (the replay thread at a submission's start) the work is split over the
replay workers: about 700 regions and 0.7 MiB in Hyrule Field, half a millisecond of the submitting thread's time on one
thread. The flush at the end is a no-op on coherent memory.

### `owners_wanted()`

The player's model ownership goes with a submission only for the motion vectors the upscaler or frame generation reads
(`runtime/vk/vk_exec_draw.c` `mvobj_draw()`): without either, nothing reads it, and the walk of the player's actor and
models in guest memory at every submission is left out.

### `vk_usnap_take()`

`NX2SARIA_REPLAY_ASYNC`, the submitting thread before the submission is queued for the replay: the uniform ranges it
binds are copied now, while they hold what the game submitted. The collected ranges are merged, and what lies wholly in
one imported pool is kept. `vk_usnap_begin()` takes the copy over when the replay reaches the submission, and leaves
out there what GPU writes in flight touch (the replay knows them only then).

### `adopt()`

The copy `vk_usnap_take()` made for this submission becomes the replay's. 0: none was taken.

The queue is in submission order, so an entry ahead of this submission's belongs to one that never reached the replay
and is freed. A region a GPU write in flight touches is marked dropped: the mirror holds those bytes, not guest memory.

### `vk_usnap_begin()`

The start of a submission's recording. With snapshots off it still prunes the write list: `vk_usnap_drop()` goes on
listing the GPU's writes then, and without the prune the list only grew. When `adopt()` finds no copy, it collects
here: the ranges are merged, keeping only what lies wholly in one imported pool and no GPU write in flight touches, and
copied with the replay workers' help.

The player's model ownership is captured here only when the replay is not asynchronous: without a snapshot from the
submitting thread, asynchronous replay must not read present-day actor ownership.

### `first_above()`

First region whose end is above `a`.

### `vk_usnap_find()`

Consecutive bindings mostly name the region the last one did: that one (`hint`) is tried before the binary search. The
regions are sorted and disjoint, so the region that holds `addr` is the one `first_above()` finds. The hint is per
thread: the replay workers look up at once.

### `vk_usnap_drop()`

`drop_lock` is taken while chunks are recorded (`vk_exec_shared`): the replay workers' shaders write storage at once.

The replay workers (`runtime/vk/vk_exec.c`) record the chunks of a group at once, and with `NX2SARIA_REPLAY_PIPE` the
submitting thread goes on past the group meanwhile. A drop holds from its place in the stream on (`vk_exec_stream_pos`):
a lookup sees the drops of the ops before it and not those of the chunks after it, in its group or later ones; the
earliest place is kept. A drop an earlier chunk has not run yet when a later chunk looks the range up is not seen, and
the later draw reads the copy instead of the earlier one's result: the chunks' drops into copied ranges are counted
(`vk_usnap`: lines) so that a scene where that can happen shows in the log. None did in Kokiri Forest, Hyrule Field or
Kakariko Village (2026-11-20): every drop into a copied range came from the submitting thread, which a group's chunks
never run beside in the stream (`tests/usnap_order_check.c` forces both orders).

### `wr_overlaps()`

A GPU write of a submission not known to be complete, at or before this place in the stream, touches [lo, hi). A linear
scan: `vk_usnap_drop()` appends after `wr_prune()` sorted the list, so the binary search of `wr_hits()` does not hold
here; only called on rare paths.

### `cpu_written()`

The same test `vk_usnap_begin()` keeps regions by.

### `vk_usnap_read()`

The bytes from the copy (1), or, for memory the CPU writes that no GPU write touches, from guest memory (2). The write
list is read under `drop_lock` while chunks are recorded: a chunk's drop may grow the list.

### `host_copy()`

The host copy where there is one (DLSS), else the device copy.

### `vk_usnap_shutdown()`

The arenas back on the free list (`vk_exec_draw_shutdown`: the device is idle, every completion ran).

### `vk_usnap_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): the free uniform snapshot arenas (the free list is never
trimmed) and the GPU-write list (the replay thread's; read without a lock, a line may be one write behind).

### `vk_usnap_motion_owner()`

Ownership travels with the uniform snapshot. A missing, GPU-written, or overlapping range is not a match.

The producer excludes overlapping identities and sorts the remaining disjoint ranges once, so the lookup is a binary
search: most scene draws are unowned, and scanning every player binding for each one would be wasted.

## `runtime/vk/vk_gpa.c`

The device address table: GPU address pages to Vulkan buffer device addresses, for the global memory accesses the
translator finds no storage buffer for (translator patch 0030: GlobalToStorage TryKeepDeviceAddressAccess, and the
SPIR-V back end's GetDeviceAddressPointer, which reads this layout). The game's foliage, instance and light data reach
their buffers through pointers the shaders read from other buffers at an index (a phi of constant-buffer pointers and
loaded ones, a pointer spilled to local memory and loaded again); those accesses read 0 before, which drew interiors and
shadowed areas black and dropped instanced foliage.

One storage buffer of 32-bit words, bound whole to every shader that declares it (res.json "table", `cb_slot` 0x40):

- Words 0-1: the device address of a zeroed 4 KiB buffer, which an address no page maps reads and writes instead.
- Word 2: the word index of the records (`REC_WORD`).
- Words 3-4: the device address of a 4 KiB scratch buffer, where a twin write goes when its page has no twin.
- Words 16..: L1: 65536 entries, one per 16 MiB of the 40-bit GPU address space, the index of its L2 block (0: none).
- Words 65552..: `BLOCKS` (1024) L2 blocks of `L2_N` (4096) entries, one per 4 KiB page, so a block is 16 MiB and the
  blocks together 16 GiB of mapped GPU address space: the record of the buffer that holds the page (0: none). Block 0
  stays zero.
- From `REC_WORD` on: `RECS` (65536) records of four words (`REC_WORDS`): the buffer's device address minus the GPU
  address of its byte 0, then the same for the page's twin (low words first). Record 0 means none.

The twin: a pool with a VRAM mirror (`runtime/vk/vk_memory.c` `mirror_create()`) has the mirror in its record's first
address and its imported buffer, guest memory, in the second; any other record has the same buffer in both. Shaders read
through the first and write through both (translator patch 0034): the GPU's writes through the table are not recorded as
GPU writes (`runtime/vk/vk_exec_draw.c` `ssbo_info()`), so with the mirror alone the CPU never saw them, and a sync of a
page the CPU wrote copied the stale guest page over them. The game's compute passes write tree and foliage records this
way, and a record overwritten so drew a tree stretched into a column reaching the sky (Hyrule Field, Kakariko, Death
Mountain). A record a page entry names is never written by the CPU: a new mapping takes a new record, and a record goes
back to the free list only once the GPU finished every submission issued while a page could still name it
(`vk_completion_add`). The one write to a record in use is the GPU's own, in queue order, when a pool's new mirror takes
its record over (`vk_gpa_flip_cmd()`): what was queued before it reads the old addresses, what comes after it the new
ones. Page and L1 entries are single 32-bit stores, so a shader that reads the table while it changes sees the old
mapping or the new one, never half of each. The CPU keeps a copy (shadow) for its reads: the GPU copy is often device
memory behind the PCIe BAR, slow to read back.

An L2 block whose last page is unmapped leaves its L1 entry at once (`blocks_release()`) and serves another 16 MiB range
once the GPU finished every submission issued while a shader could still have read the old L1 entry: until then it holds
only zeros, which read as unmapped. The game's allocator hands out pool addresses that climb through the 64 GiB alias
region, so blocks kept for good ran out after 16 GiB of ranges ever mapped (39 minutes of play, six areas), and every
pool created after that stayed out of the table: its pointers read zeros, as before the table existed. A range mapped
again while its block still waits takes that block back (`block_take()`), so a pool freed and made again at its address
within a frame holds no second block.

`NX2SARIA_GPA=0` turns the table off: the translator is told the host has no device addresses (those accesses read 0).
`NX2SARIA_GPA_EMPTY=1` keeps it empty, so every access through it reads the zero buffer (diagnosis). Mirrored pools are
not synced for the table's readers (`runtime/vk/vk_exec_draw.c` `ssbo_info()` says why). With shadow storage the pools
are copies of guest memory, which a guest submission that binds the table (`runtime/vk/vk_exec_draw.c` `ssbo_info()`)
brings up to date with the pages the CPU wrote, once, before the part is submitted
(`runtime/vk/vk_memory.c` `vk_mem_shadow_sync()`). There the twin of a mirrored pool is its shadow copy, not guest
memory, and what a shader stores through the table is not noted as a GPU write: it does not come back to guest memory,
and the next whole copy-in of its page overwrites it. The notes of `runtime/vk/vk_memory.c` on the pages a shader stores
into through the device address table have the sequence, what was measured, and the switch that keeps the stores
(`NX2SARIA_SHADOW_TABLE_STORES`).

The state, `G`:

- `state`: 0 untried, 1 on, -1 off. `gpu` is the mapped table, `cpu` its shadow.
- `nblocks`: L2 blocks ever handed out (block 0 is the zero block). `blk_pages`: the pages each block maps. `blk_l1`:
  the L1 entry that names a block, or named it while it waits. `blk_wait`: off L1, waiting for the GPU
  (`blocks_release()`). `blk_gen`: a block's releases so far, as a block taken back and released again has two
  callbacks. `free_blks`: empty blocks whose fence completed. `blk_returned`: blocks taken off L1 so far.
- `nrec`: records handed out, free ones aside. `free_recs`: records whose fence completed.

### `create()`

The table is a transfer destination as well as a storage buffer, for the store `vk_gpa_flip_cmd()` records.

### `block_take()`

A block for L1 entry `l1`, 0 when none is left: the one the entry gave up and that still waits for the GPU (it maps
nothing, and a shader that read the old entry finds the new pages in it), else a free one, else one never used. A block
taken back is no longer marked as waiting, so its callback finds it in use (`blk_free_cb()`).

### `page_word()`

The L2 entry word of a page; 0 when its block does not exist and `alloc` is 0 or no block is left. The L1 index is the
page number shifted by 12: 4096 pages of 4 KiB per 16 MiB.

### `page_put()`

Stores record `r` (0: none) in the L2 entry word `w` and keeps the page counts of its block and of the table.

### `blk_free_cb()`

Frees the block only when it still waits and its generation is the callback's: not taken back since (`block_take()`).

### `blocks_release()`

After pages of [p0, p1) were unmapped: the blocks of that range that map nothing leave L1, and go back to the free list
once the GPU finished the submissions issued until now and the one being recorded, as a record does.

### `out_of_blocks()`

`gpa_lock` held: a page found no block. Pointers into it read the zero buffer until its buffer is mapped again.

### `rec_alloc()`

The record is written before any page names it: the caller's page stores are ordered after the record's words by the
release fence at the end.

### `vk_gpa_map()`

Maps the pages the buffer holds whole: [lo, hi) rounded inwards (imports are aligned outwards, so this is all of them).
With `NX2SARIA_GPA_EMPTY=1` nothing is mapped and every access reads the zero buffer (diagnosis). A page that finds no
block skips the rest of its 16 MiB, which has no block either; the next range may have one.

### `vk_gpa_unmap()`

Pages another live buffer also holds whole (overlapping imports of the same guest memory) get that buffer's record back
through `cover`. The blocks are released after the cover, so that a block another buffer's pages went back into stays.

### `twins_on()`

`NX2SARIA_GPA_TWIN=0` (diagnosis): no twin, the writes go to the mirror alone, as before patch 0034.

### `rec_first()`

`gpa_lock` held: the first address of record `rec` (the buffer its pages read).

### `pages_move()`

`gpa_lock` held: the pages of [p0, p1) that name `old_rec` (or none) name `r` from here on.

### `remap()`

With `keep_twin` and twins on, the new record's twin is the buffer the pages named before (the pool's imported buffer),
so writes reach guest memory too. When no record is left, the pages keep the old one, and the caller its number.

### `vk_gpa_flip_cmd()`

Records into cb the store that hands record rec over to buf, a pool's new VRAM mirror: from that point of the queue on
the pages that name rec read buf, and their twin is the buffer they read until then (`keep_twin`, with twins on) or buf
again. A change of the table made on the CPU is seen at once by every shader still running, the ones queued before the
mirror's first copy included, which would then read or write the mirror before anything had filled it; this store runs
where the command stands, after that copy (`runtime/vk/vk_memory.c` `vk_mem_mirror_prologue()`). The caller puts
barriers around it. No new record is taken, so there is none to run out of. The shadow takes the new words now: the CPU
never reads a record's addresses back for a record handed over this way. 0 when nothing was recorded.

### `vk_gpa_probe()`

For `--probe-vkmem`. What the table's own memory, as the GPU has it, resolves addr to: the device address a shader reads
there and the one it writes as well. 0 when no page maps addr. Slow (the table may be device memory), and only
meaningful with no submission in flight.

### `vk_gpa_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): device address table records and L2 blocks in use, against
their fixed sizes (when they run out, shaders read zeros). A record that waits for its fence still counts as in use; a
block that waits for the GPU does not.

## `runtime/vk/vk_gpa.h`

The device address table (`runtime/vk/vk_gpa.c`): GPU address pages to buffer device addresses for shaders' global
memory accesses.

`VK_GPA_CB_SLOT` is res.json's `cb_slot` of the table (the translator's ResourceManager.DeviceAddressTableCbSlot).

- `vk_gpa_wanted()`: the device has buffer device addresses and `NX2SARIA_GPA` is not 0. `vk_gpa_wanted_ctx()`: the
  same during device creation, before `vk_ctx()` returns the context.
- `vk_gpa_on()`: the table exists (created on first use). `vk_gpa_info()`: the whole table, for a storage descriptor.
- `vk_gpa_map()`: maps the pages [lo, hi) holds whole to `buf`, whose byte 0 is GPU address `buf_lo`. Returns the
  record (0: not mapped).
- `vk_gpa_unmap()`: unmaps the pages of [lo, hi) that name `rec`; `cover` (may be NULL) gives the record of another
  live buffer that holds a page whole, or 0. `rec` is reused once the GPU finished the submissions issued until now.
- `vk_gpa_clear()`: unmaps every page of [lo, hi), whatever record it names (pages of a virtual pool whose physical
  memory goes away).
- `vk_gpa_remap()`: moves the pages of [lo, hi) that name `old_rec` (or none) to `buf` (a pool's VRAM mirror), with
  the buffer of `old_rec` as their twin, which shaders write too; returns the new record. `vk_gpa_remap_plain()`: the
  same without the twin: the pages name `buf` alone (a pool whose VRAM mirror is evicted goes back to its imported
  buffer).
- `vk_gpa_flip_cmd()` and `vk_gpa_probe()`: under `runtime/vk/vk_gpa.c` above.

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
