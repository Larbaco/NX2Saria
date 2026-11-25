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
- `}`: vkGetDeviceFaultInfoEXT needs the feature, not just the extension.
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
  driver for RDNA 2 and older, see `vk_caps_gpl`): `vk_caps.gpl` = 0, the extension stays disabled, and pipelines are
  built whole.
- No 32-lane pin (no `subgroupSizeControl`, 32 outside min..max, or `NX2SARIA_VK_SUBGROUP_PIN=0`): stages run at the
  device's `subgroupSize` (SPIR-V 1.3 without a required size) and the translator is told that size.
  `NX2SARIA_VK_SUBGROUP=<n>` pins n instead of 32 where the driver allows it.

The defaults (Float64, no storage rounding, 32 everywhere) leave the shader cache keys as they were
(`runtime/gfx/gfx_shader.c`).

- `gpl_lib`: the driver's `graphicsPipelineLibrary` (feature), ...FastLinking (property).
- `gpl_off`: why gpl is off, for the boot report; NULL when it is on.
- `summary`: `vk_caps_summary`: written once, in `vk_caps_init`, before any other thread reads it.
- `gpu`: The card for the session summary (`runtime/session_summary.h` `vk_caps_gpu()`): stored once in `vk_caps_init`,
  published by `gpu_known`.
- `amd_proprietary`: AMD's own Windows driver (any branch): `runtime/gfx/gfx_shader.c` `patch_vec_index()`.

### `vk_caps_vec_index_bug()`

`runtime/gfx/gfx_shader.c` `patch_vec_index()`: the driver reads uniform vector components at computed indices wrongly
(the translator's HasVectorIndexingBug, which is set for AMD's Windows drivers).

### `vk_caps_driver()`

- AMD split RDNA 1 and 2 off into a driver branch of their own; its `driverInfo` reads "26.10.1 (AMD proprietary shader
  compiler)" where the RDNA 3 and 4 driver names LLPC.

### `vk_caps_gpl()`

`NX2SARIA_GPL=0` turns graphics pipeline libraries off and 1 turns them on; unset, they are on wherever the driver has
fast linking, except on AMD's Windows driver for RDNA 2 and older (`vk_caps.amd_sc`: RX 400 to RX 6000). On an RX 6700
XT the game ended with code 71 at every enemy killed, from 0.5 on, and no longer with gpl = 0; an RX 5700 XT showed the
same ending (issue `#418`). Where in the driver it faults is not established: a kill draws its effects for the first
time, which asks for a burst of new pipelines linked from stage libraries, and building them whole avoids it. The price
there: with libraries off a pipeline the game needs for the first time is built whole by a worker while its draw is left
out (`runtime/vk/vk_exec_draw.c`, "pipeline still building"), so new things appear some frames late, and the start waits
for every recorded pipeline instead of 20 s at most (`runtime/vk/vk_pipeline.c` `prewarm_run()`). The same driver had
libraries off before, for device losses at boot (issue `#5`), whose cause was GPU writes past 3D BC images
(`runtime/vk/vk_exec.c`, "3D BC render targets"); it boots with them since that was fixed, so gpl = 1 is there to try on
a newer driver. RDNA 4 under RADV is reported to hang the GPU at the title screen with GPL.

### `vk_caps_init()`

- Gathers (TLD4) pick their four texels with Maxwell's rounding; AMD and Intel round a coordinate that sits between two
  texels the other way, which draws dark lines along object edges in shadow-map filtering ([maxwell.md](../maxwell.md)
  2.10; seen on AMD). The translator biases gather coordinates by the host's sub-texel precision; NVIDIA rounds as
  Maxwell does. `NX2SARIA_GATHER_BIAS=<bits>` forces it (0: off).
- A register read before any write is an undefined load in SPIR-V, so function-local variables start at 0
  ([maxwell.md](../maxwell.md) 3.12). Mesa folds undefined values freely; NVIDIA's compiler gives such a read a value of
  another lane of the warp: Ocarina's clustered-light loop reads registers that only some paths write, and on an RTX
  5090 the Great Fairy Fountain floor took an extra light term in 4x8-pixel tiles (one 32-lane warp each) and Link went
  dark with green speckles. On with every setting: `runtime/vk/vk_dlaa.c` knows the game's FXAA by its zeroed SPIR-V
  hash too, and the shader cache keys (which include this) stay the same whether DLSS is on or off, so shaders prepared
  with one setting serve the other. `NX2SARIA_ZERO_LOCALS=0` turns it off.
- bda: global accesses the translator finds no storage buffer for go through the device address table
  (`runtime/vk/vk_gpa.c`).
- `for (uint32_t h = 0; h < c->mem_props.memoryHeapCount; h++)`: the largest device-local heap: the card's own memory.

## `runtime/vk/gpu_choice.h`

The graphics card the settings choose (`gpu_index`, `gpu_name`) and what the log says about the choice:
`runtime/vk/vk_device.c` `pick_device()` and `fatal_note()`, `runtime/cfg/hw_detect.c` `probe_gpu()`, ui/menu.cpp
`row_gpu`. Header only, and plain enough for C and `C++`, so that `tests/gpu_choice_check.c` checks the rule and every
line without a device.

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

- `enum { GPUC_OTHER, ... }`: VkPhysicalDeviceType's values, so that this header needs no Vulkan header.
- `GpucCard`:
  - `const char *name`: the name the driver gives the card.
  - `int type`: `GPUC_*`.
  - `int usable`: the game can run on it: Vulkan 1.3, its extensions, a queue that presents. Read for discrete cards
    only: `gpuc_legacy_alt`, `gpuc_choice_line` and `gpuc_prepare_line` look for one the game can use.
- `gpuc_has()`: 1 when part is in s, upper and lower case alike: `gpu_name`'s match.
- `gpuc_index()`: The position to go by, for `gpu_index` want (-1: unset) and `gpu_name` name (NULL or empty: unset)
  over the n cards listed now: want, unless a name is set and the card at that position does not have it (the cards are
  listed in another order than when `gpu_index` was written, or one of them is gone). Then -1, `*dropped` is 1 and the
  name finds the card. Nothing is dropped while one of the two is unset.
- `gpuc_legacy_applies()`: 1 when a `gpu_index` alone lists a card that is not a discrete one, the part of
  `gpuc_legacy_alt` that needs no card's usable: want (-1: unset) over the n cards listed now lists a card that is not a
  discrete one, no `gpu_name` is set (a name is the player's own choice of that card), and the index is not the
  environment's (`from_env`: set for this run, on purpose). A caller that has to find out which cards the game can use
  does so only when this is 1.
- `gpuc_legacy_alt()`: The discrete card a `gpu_index` alone gives way to, or -1 when the position stands: the first
  discrete card the game can use, when `gpuc_legacy_applies`. Nothing to give way for with Auto, a position past the
  list, a discrete card at it, a name, an environment setting, or with no discrete card to use (the card at the position
  is then the only one that runs the game).
- `gpuc_resolve()`: The position the pick goes by for the settings want and name (as `gpuc_index` and `gpuc_legacy_alt`
  take them), the one call for the device choice (`runtime/vk/vk_device.c` `pick_device()`) and the preset's probe
  (`runtime/cfg/hw_detect.c` `probe_gpu()`), so that the preset is chosen for the card the game takes: `gpuc_index`'s
  result, or -1 when a `gpu_index` alone gives way to a discrete card. `*dropped` is `gpuc_index`'s, `*alt`
  `gpuc_legacy_alt`'s (-1: the position stands); the caller says either in the log.
- `gpuc_shown()`: The card the settings pick as far as the list tells, for the menu's Graphics card row: the one
  `gpu_index` lists when it has the name (or no name is set), else the first with the name; -1 for Auto, for a position
  past the list, for a name that no card has (the game then takes the best card, as for Auto) and for a `gpu_index`
  alone that gives way to a discrete card (`gpuc_legacy_alt`; `from_env`: the index is the environment's).
- `gpuc_setting_line()`: The line at each start that says what the two settings hold over the cards listed now, agreeing
  or not, with want and name as the settings file gives them (-1 and NULL or empty: unset), and `from_env` as for
  `gpuc_legacy_alt`.
- `gpuc_dropped_line()`: The line for a `gpu_index` that `gpuc_index` dropped, with the want it was given: what it
  listed, and that the name decides.
- `gpuc_legacy_line()`: The line for a `gpu_index` alone that gives way to Auto (`gpuc_legacy_alt`), with the want it
  was given and the card alt it returned: what the position listed, and what to do to run on that card on purpose (the
  settings file's `gpu_name` too, for a game that cannot open its menu).
- `enum { GPUC_BY_AUTO, ... }`: How the card the game runs on was chosen, for `gpuc_prepare_line`.
- `gpuc_prepare_line()`: The text the shader preparation modes print after "prepare-shaders: card: "
  ("prepare-pipelines: card: " in that mode; `runtime/vk/vk_pipeline.c`): the card whose driver cache they build, picked
  (an index into c) by how, which is a `GPUC_BY_*` value, with want and name as the settings give them. Said in the
  launcher's log, where a player's report of a slow update is read: the preparation's own device lines are among sixty
  others. Where the card is not a discrete one while a discrete one the game can use is listed (the player's own choice
  of the card by its name, or an index in the environment), it also says that one's driver cache is not the one built.
- `gpuc_choice_line()`: The card the settings picked (want, name: what the pick went by after `gpuc_index`, -1 and NULL
  for Auto) is not a discrete card while one the game can use is listed, and it was chosen by a `gpu_index` alone: the
  first such card is returned, line gets the warning for the log and note (NULL: none wanted) the short form for a fatal
  error's report. A position says little over the cards' order changing from one start to the next. A `gpu_name`, which
  the menu writes beside the position, is the player's own choice of that card, so it gets no warning at each start.
  Returns -1 and empties both when there is nothing to say: Auto, a `gpu_name` set, a discrete card picked, or no
  discrete card to use.

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
