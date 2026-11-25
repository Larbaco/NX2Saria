# runtime/vk: upscalers, anti-aliasing, frame generation and the passes after the game's frame

What the Vulkan back end adds to the game's own frame: the upscalers and anti-aliasing (FSR 1, FSR 3.1, FSR 4, DLSS and
DLAA, SMAA), frame generation (FSR 3.1, FSR 4, DLSS Frame Generation), the motion vectors and camera they need, and the
passes that run on the finished picture (contact shadows, tone mapping, HDR output).
[README.md](README.md#the-pages-of-runtimevk) lists the files. A header's section holds what its callers are promised;
how a function does it is in the section of the file that defines it.

What the features are for and how a player sets them is in [frame-generation.md](../frame-generation.md),
[performance.md](../performance.md), [architecture.md](../architecture.md) and the lines of
[logging.md](../logging.md); the library behind DLSS Frame Generation on Windows is on [dlssg.md](dlssg.md), and the
shaders these files run are on [shaders.md](shaders.md).

## `runtime/vk/vk_fsr.c`

AMD FidelityFX Super Resolution 1 on the present path (`runtime/gfx/gfx_present.c` `present_image()`): when the window
is larger than the game's output, EASU scales the game's crop rectangle to the swapchain size and RCAS sharpens it, in
place of the bilinear `vkCmdBlitImage`. When the game's output is larger than the window (the game draws 2560x1440
docked, so a 1920x1080 window shrinks it), EASU does not apply (it only enlarges): a bilinear blit shrinks the crop into
the first intermediate image and RCAS sharpens that, which gives back some of the detail the shrink blurs.
`NX2SARIA_UPSCALER` (`fsr` or `bilinear`) picks it, `NX2SARIA_SHARPNESS` (0..100, 0 = no RCAS pass) sets RCAS; both
apply at the next frame.

Per frame: EASU reads the source through a view of its own format (the same colour conversion the blit does) and writes
an RGBA16F image of the picture's size on the swapchain (all of it, or the part between black bars,
`runtime/gfx/gfx_present.c` `gfx_fit_rect()`); RCAS reads that and writes a second one; a 1:1 blit converts the result
into its place in the swapchain image, which the caller left in `TRANSFER_DST`. The shaders
(`runtime/shaders/fsr_*.comp`) compute AMD's constants from the push constants. Cost: two compute passes over the
swapchain size and one copy per present; nothing on the game's draws.

The source view of slot s is destroyed when slot s records again: `present_image()` has waited for that slot's fence.
The two intermediate images are shared by all slots; each frame's first barrier orders it after the previous frame's
reads (one queue, so the barrier covers earlier submissions).

The state, `F`:

- `img`: EASU output, RCAS output.
- `w`, `h`: their size.
- `src_view`: the source view each slot's last frame read.
- `used_slots`, `since`: slots whose frames used the images / that recorded without them since the last use.
- `idle`: presents recorded without them since their last use.

### `idle_note()`

The two images while the upscaler is not fsr (or the picture needs no scaling): they stayed for the rest of the run, two
RGBA16F pictures of the window's size, 168 MiB at 5120x2142. They go once `IDLE_FREE` (600) presents recorded without
them and every slot that ever used them has recorded since: the caller waited for that slot's fence before it recorded,
so no submission still reads them. Taking fsr up again makes them as a window size change does (`ensure_images()`: a
wait for the idle device and two images).

### `ensure_images()`

A swapchain size change: earlier presents may still read the old images. `vkDeviceWaitIdle` needs every queue externally
synchronized, so it holds the queue lock like a submission.

### `shrink_sharpen()`

The shrink path: the crop of src (GENERAL) shrunk by a bilinear blit into `F.img[0]`, RCAS into `F.img[1]`, a 1:1 copy
into dst. Returns 2. The barriers are those of `vk_fsr_record()`.

### `vk_fsr_record()`

- EASU only enlarges: a picture smaller than the crop in either direction takes the shrink path, which without
  sharpening is the plain blit of the caller.
- The game's writes to the source become visible to the compute read (in the shrink path, to the blit); the earlier
  frame's reads of the intermediates finish before this frame writes them.
- Later submissions that write the source wait for the compute read.

## `runtime/vk/vk_fsr3.h`

What `runtime/vk/vk_fsr3.c` promises `runtime/vk/vk_ffx.cpp`, which routes `vk_ffx_upscale()` there in the builds that
load no `amd_fidelityfx_vk.dll` (all but Windows).

- `vk_fsr3_init()`: 1 when the device can run the passes (16-bit storage image formats, quad subgroup operations in
  compute shaders) and their pipelines were made; made once, on the first call after the device exists. 0 before it
  exists.
- `vk_fsr3_available()`: before `vk_fsr3_init()` ran: 1 (the build has the passes); after, what it found. Any thread
  (the settings menu).
- `vk_fsr3_upscale()`: the contract of `runtime/vk/vk_ffx.h` `vk_ffx_upscale()`; every image also carries a view
  (`VkFfxImage.view`). sharpness 0..1, RCAS after the upscale when sharpen is set. Returns 0 when it recorded nothing.
- `vk_fsr3_release()`: before the device goes away (the GPU idle): everything, the contexts let go of too.
- `vk_fsr3_holds()`: the upscaler is off (`vk_ffx_release_idle()`): whether a context is made, and lets them all go (the
  caller's `vk_upscaler_let_go()` frees them once the GPU is past them); the next upscale makes one again. The pipelines
  stay.
- `vk_fsr3_dead_take()`: the contexts let go of, the part of `runtime/vk/vk_ffx.cpp` `vk_ffx_dead_take()` and
  `vk_ffx_dead_fence()` outside Windows: those whose last upscale's submission is at most done are destroyed (their
  count); the highest such submission among those left (0: none).
- `vk_fsr3_contexts_made()`: contexts made so far (the probe: what a change of render size makes).

## `runtime/vk/vk_fsr3.c`

AMD FSR 3.1 temporal upscaling recorded by the runtime: the upscaler's compute passes (`runtime/shaders/fsr3_*.comp`,
over the headers in `runtime/shaders/ffx3/`, built into `runtime/fsr3_*_spv.h` by `tools/build_shaders.py`).
`runtime/vk/vk_ffx.cpp` sends `vk_ffx_upscale()` here in the builds that load no `amd_fidelityfx_vk.dll` (all but
Windows). Frame generation is not here: on Windows it runs through that library's own API calls
(`runtime/vk/vk_fg.cpp`), and elsewhere through `runtime/vk/vk_fsr3fg.c`.

The inputs are `runtime/vk/vk_dlaa.c`'s, the ones `runtime/vk/vk_ffx.cpp` hands the library: the scene light (linear
HDR), its D32F depth (OpenGL-style, 0 near and 1 far, with the frame's near and far planes; not inverted), motion
vectors at the render size in render pixels from the current frame to the previous one, without the jitter, and the
jitter in render pixels. The passes are built with the options of `runtime/shaders/fsr3_options.h` for those inputs
(HDR, low-resolution motion vectors, depth not inverted) and run with auto exposure. Every image is in GENERAL and stays
there.

One call (`vk_fsr3_upscale()`) records, in this order: the clears (history on a reset, the reconstructed depth and the
SPD mips every time), then prepare inputs (dilated depth and motion, the previous frame's nearest depth reconstructed,
luma, farthest depth), luma pyramid (SPD: auto exposure and the farthest depth at half size), shading change pyramid
(SPD again over the luma change), shading change, prepare reactivity (disocclusion, locks, accumulation), luma
instability, accumulate (the upscale itself) and, with sharpening, RCAS on the accumulated colour into the output. A
global compute barrier separates the passes. `fill_constants()` computes the constants the passes read; the history
images alternate between calls.

Contexts (`ctx_for()`): one per render size and output size, or with the game's dynamic resolution one per output size
for every render size up to it, at most `CTX_MAX`; one no upscale used for 90 seconds goes, and all of them go while the
upscaler is off (`vk_fsr3_release_idle()`). A context that goes is let go of (`retire_ctx()`) and its images are
destroyed once the GPU is past the submission of its last upscale (`g_dead`; the list is `runtime/vk/vk_dlaa.c`
`upd_put()`'s), as `runtime/vk/vk_ffx.cpp` does with the library's contexts. The constants live in a host-visible ring
of `RING` (256) slots, each one `Consts`, `SpdConsts` and the RCAS words; a slot is reused `RING` calls later, after the
GPU passed the submission that last read it.

### The images of a context

- `I_ACC0, I_ACC1`: accumulation (locks' age), this call's and the last one's, alternating.
- `I_LUMA0, I_LUMA1`: current and previous luma, alternating.
- `I_LH0, I_LH1`: luma history (four frames), alternating.
- `I_UP0, I_UP1`: the upscaled colour, alternating.
- `I_INTER`: farthest depth, then luma instability: one image, which the first has left by the second.

`I_SPD` (R16G16F, half size) has every mip; the SPD passes write mips 0-5 through views of their own (`SPD_VIEWS` 6).

### What the passes bind

The binding numbers of `runtime/shaders/fsr3_*.comp`.

`S_*`: sampled images.
`U_*`: storage images.
`C_*`: uniform buffers.

`Consts`: the constants of every pass but RCAS and the SPD ones (std140 `cbFSR3Upscaler`: all 4-byte members, in this
order).

### State

`CTX_RECENT` (8 calls): a context used this recently may sit in a command buffer not yet submitted.

- `CTX_IDLE_NS`: a context no upscale used for this long goes (`vk_fsr3_upscale()`): a second scene's (the pause
  screen's) a while after it closed, or one of an output size the window no longer has. Unused that long, no command
  buffer names it.

`Ctx`:

- `mw`, `mh`, `ow`, `oh`: the render size it was made for (the largest it takes with drs); the output size.
- `drs`: made for dynamic resolution: every render size up to mw x mh.
- `used`, `used_ns`: `G.tick` and the time when `ctx_for()` last returned it.
- `fence`: the submission of its last upscale (`vk_exec_recording_fence()`), 0 none.
- `fresh`: the images were never recorded into: transition and clear them first.
- `odd`: which of the alternating images this call writes.
- `k`: carried from call to call (previous sizes, jitter, frame index).
- `pre_exposure`: the previous call's.

`G`:

- `state`: -1: not tried; 0: off; 1: ready.
- `VkImage lut, zero; VmaAllocation lut_mem, zero_mem; VkImageView lut_view, zero_view`: shared by every context: the
  Lanczos table and the 1x1 masks that stand for the reactive and transparency ones.
- `lut_src`: the table's upload source, kept (256 bytes).
- `slot_bytes`, `block`: one ring slot, and one block of it (the uniform buffer offset alignment).
- `ring_fence`: the submission that last read each slot (0: none).
- `made`: contexts made so far (`vk_fsr3_contexts_made()`).

`g_dead`: the contexts let go of (`retire_ctx()`), destroyed once the GPU is past their last upscale
(`vk_fsr3_dead_take()`, which `runtime/vk/vk_ffx.cpp` `vk_ffx_dead_take()` calls when `runtime/vk/vk_dlaa.c` takes what
the upscalers let go of). Each entry's handle is a heap copy of the context (its images and views). Only the thread that
makes the upscales (`runtime/vk/vk_dlaa.c`'s pass hooks) touches it; `vk_fsr3_release()` at teardown.

### `make_pass()`

- `s_PointClamp` and `s_LinearClamp` of the passes' headers, fixed in the layout: nothing to push for them.

### `make_shared()`

- The reprojection's Lanczos-2 weights over [0, 2] in 128 steps, as 16-bit signed normalised values.
- The constant ring.

### `vk_fsr3_init()`

- Without a device it returns 0 and is asked again once the device exists.
- The present's thread (`runtime/vk/vk_dlaa.c` `vk_dlaa_frame()`) and the replay's (`evaluate()` there) may both ask
  first: one makes the passes, the other finds them not ready yet, as before the device existed.

### `free_ctx()`

A context's images and views destroyed at once: for one no submission names (half made), one the GPU is past
(`dead_release()`) and teardown (the device idle).

### `dead_release()`

One entry of `g_dead`, as `upd_take()` hands it out, timed as "upscaler releases" (`NX2SARIA_TRACE=perf`).

### `retire_ctx()`

A context let go of while the GPU may still run a command buffer that names it: its images and views go once the GPU is
past the submission of its last upscale (`g_dead`), and the slot is free at once. The caller calls
`vk_upscaler_let_go()` after it (or its own caller does: `runtime/vk/vk_dlaa.c` `retire_idle()` after
`vk_fsr3_release_idle()`).

- No room for the entry: the images go to `runtime/vk/vk_memory.c`'s deferred destruction, stamped with the submission
  being recorded, which comes after every submission that named them.
- Full, which the GPU's progress does not let happen: room after a wait for the device, or else all of them go, the
  device idle.

### `make_ctx()`

The images for render sizes up to mw x mh (the passes take the size of each call from the constants and read only that
part of an image) and the output size ow x oh.

- A small scene has fewer mips than the passes name: the views past its last mip name that last one.
- The constants that stay for the context's life: the largest sizes and the passes' tuning at its defaults.

### `ctx_for()`

One context per render size and output size, as `runtime/vk/vk_ffx.cpp` `context_for()` keeps the library's: a render
size of its own starts a new history, as the library's new context does. With the game's dynamic resolution on
(`NX2SARIA_DYN_RES`, `runtime/pacing/dynres.c`), whose steps change the render size every few seconds, one context per
output size takes every render size up to the output's, each call's own size coming from the constants. A context of
another render size goes once no command buffer still being recorded can name it (`CTX_RECENT`), and the game's second
scenes, of another output size, keep their own. When all are taken, the least recently used one goes for the new size. A
context that goes is let go of (`retire_ctx()`): the GPU may still run a command buffer that names it. Images that
cannot be made while contexts let go of are still in flight are tried once more after the GPU is past those and they are
destroyed.

The contexts of this output size for another render size go once not recent ("the render size changed"), and the least
recently used one when its slot goes to another output size; the work of either may still be in flight.
- A context is made for the render size itself; for dynamic resolution, every size up to the output's (and a scene at
  the output size).
- Refused while contexts let go of (this file's and the other upscalers') are still in flight: most often their video
  memory. The GPU finishes with them (`vk_upscaler_gpu_wait()`: those submissions only, not the device), they go, and
  the images are made once more, as `runtime/vk/vk_ffx.cpp` `context_for()` does for the library's contexts, rather than
  the upscaler failing.

### `fill_constants()`

The constants of this call: sizes, jitter, the depth to view-space conversion of the camera's near and far planes (depth
not inverted, 0 at the near plane; a projection without a far plane has `VK_DLAA_FAR_INFINITE` there, far enough to be
the same), the frame index and the jitter phase count, which steps towards its new value one phase per call.

- `tan_half_fov` is the tan of half the horizontal angle.

- View depth from device depth d: `device_to_view[1]` / (d - `device_to_view[0]`), with q the projection's far / (near -
  far).

- The pre-exposure is always 1.
- `mv_scale` takes render pixels to UV; `mv_jitter_cancel` is 0, the vectors carry no jitter.

### `half_bits()`

IEEE half of a float in (0, 1] (the RCAS constant), rounded to nearest.

- Below the halves' normal range it gives 0 (never here).
- The dropped bits round half up.

### `ring_slot()`

The slot was last read `RING` calls ago, done long since; it waits for that submission (at most 100 ms) if it is not.
The slot's new fence is the submission being recorded (`vk_sync_issued()` + 1).

### `vk_fsr3_upscale()`

- The context's fence is taken first: the command buffer names its images from there on.
- The frame's camera, or where it is not known the values `runtime/vk/vk_ffx.cpp` `vk_ffx_upscale()` gives the library
  then.
- What drew the inputs, and the last call's reads of what this one writes, come first.
- A fresh context: every image cleared once, so nothing reads what memory held before.
- On a reset: the accumulation this call reads is cleared, and auto exposure starts again (the frame info cleared to -1,
  1).
- Every call: the reconstructed nearest depth (to the far value, as bits of 1.0f), the SPD mips and their counter.
- The constants: this call's slot of the ring.
- SPD over the render size (the luma and shading change pyramids).
- The SPD mip count is floor(log2(max(w, h))).
- RCAS: the slider's 0..1 as stops of sharpness reduction (0 = most), packed as the pass reads it.
- The second RCAS word is the same as a pair of halves, for a 16-bit build of the pass.
- Auto exposure: the frame info image is the exposure the passes sample.
- The caller's barrier (`runtime/vk/vk_dlaa.c` `evaluate()`) orders its reads of the output after these writes.

### `vk_fsr3_release()`

Teardown and the probe: the device is idle.

- The state goes back to "not tried": a new device makes everything again.

## `runtime/vk/vk_fsr3fg.h`

What `runtime/vk/vk_fsr3fg.c` promises its callers: `runtime/gfx/gfx_present.c`'s frame generation presenter (the one
DLSS Frame Generation through NGX uses) records the passes into each present's command buffer and shows the generated
frame between the frame before and this one. Every build but the Windows one, where `NX2SARIA_FRAMEGEN` = fsr3 goes
through AMD's library (`runtime/vk/vk_fg.cpp`).

- `vk_fsr3fg_build()`: 1 in the builds where `NX2SARIA_FRAMEGEN` = fsr3 runs through these passes (all but Windows).
- `vk_fsr3fg_init()`: makes the pipelines once the device exists (0 before it, and asked again then). 1 when the device
  runs them; the first call logs which, and why not.
- `vk_fsr3fg_available()`: for the settings menu, any thread: 1 before `vk_fsr3fg_init()` ran (the build has the
  passes), then what it found, and 0 after a failure in this run (`vk_fsr3fg_failed()`).
- `vk_fsr3fg_failed()`: a context or a recording failed in this run: frame generation stays off, as logged.
- `vk_fsr3fg_on()`: `NX2SARIA_FRAMEGEN` = fsr3 now (the menu switches it live), in a build of `vk_fsr3fg_build()`, where
  it is available.
- `vk_fsr3fg_reset()`: the next frame starts without history (a resize, the menu, a pause).

- `vk_fsr3fg_generating()` is declared in `runtime/vk/vk_fg.h`, for `runtime/pacing/frame_rate.c`.

- `VkFsr3fgInputs`: one frame's inputs: the scene's depth (D32F, 0 near) and motion vectors (R16G16F, render pixels,
  current to previous, without the jitter), both of the render size and with views, the jitter in render pixels, whether
  the history broke at this frame (a camera cut, another scene size or target), the camera and the time since the last
  frame.

- `vk_fsr3fg_record()`: records the generation of the frame between the previous call's picture and this one into out
  (the presenter's image, bb's size and format, storage, GENERAL). bb: this frame's picture with the HUD; hudless: the
  same without it (NULL: none this frame, bb stands in for it); rect: the picture's rectangle in bb {x, y, w, h} (black
  bars around it); in: the frame's depth and motion. Every image in GENERAL. Returns 1 when out holds a frame to show, 0
  when not (a history that starts at this frame shows no generated frame; a failure, as logged).
- `vk_fsr3fg_evaluate()`: the same with this frame's inputs from `runtime/vk/vk_dlaa.c` `vk_dlaa_fg_inputs()`,
  `vk_dlaa_fg_views()` and `vk_dlaa_ffx_camera()`; a frame without them generates nothing and the next one starts a new
  history.
- `vk_fsr3fg_presents()`, `vk_fsr3fg_paused()`: for the periodic line both FSR 3.1 paths write
  (`runtime/vk/vk_fg_stats.h`): the presenter's presents so far, given before each frame, and a frame the presenter does
  not generate from (menu: the settings menu is open; else the game gave no picture for it), counted as
  `vk_fsr3fg_evaluate()` counts the others. The present path, `g_pl` held.
- `vk_fsr3fg_idle()`: frame generation went off (the menu): its images go once the GPU is idle; the pipelines stay.
  `g_pl` held.
- `vk_fsr3fg_release()`: before the device goes away (the GPU idle): everything, and init runs again after.

`VkFsr3fgStats`, what it did so far, for the presenter's log line and the probe:

- `frames`: calls of `vk_fsr3fg_record()`.
- `generated`: those that left a frame to show.
- `cuts`: those that started a new history (no frame shown).
- `no_inputs`: `vk_fsr3fg_evaluate()` calls without the frame's depth and motion.
- `no_hudless`: frames generated without the picture without the HUD (bb stood in).
- `gpu_ms_of`, `gpu_ms_fi`: average GPU time of the optical flow and of the interpolation (0: not measured).
- `gpu_samples`: frames those averages are over.
- `vram`: bytes of the context's images.
- `vk_fsr3fg_pass_ms()`: `NX2SARIA_FSR3FG_PASS_TIMES=1` (diagnosis, the probe): pass i's name, average GPU time a
  dispatch (from the time the dispatch before it ended) and dispatches timed; 0 past the last pass.

## `runtime/vk/vk_fsr3fg.c`

AMD FSR 3.1 frame generation recorded by the runtime: the optical flow passes (`runtime/shaders/fsr3of_*.comp`) and the
frame interpolation passes (`fsr3fi_*`.comp), over the headers in `runtime/shaders/ffx3/`, built into
`runtime/fsr3of_*_spv.h` and `fsr3fi_*_spv.h` by `tools/build_shaders.py`. No library is loaded: this is what makes
`NX2SARIA_FRAMEGEN` = fsr3 run outside Windows, on any GPU the passes run on.

One call (`vk_fsr3fg_record()`), at the end of a present's command buffer, records in this order:

- Optical flow on the picture without the HUD (the current one against the one before): its luma, the luma pyramid, the
  scene change detection (a histogram and its divergence from the frame before's), then from the coarsest of seven
  levels down to the finest a block search, a filter and (but at the finest) the scale of the vectors into the next
  level; the finest level's filter writes the vectors frame interpolation reads (8x8 blocks of the picture);
- Frame interpolation's preparation from the frame's depth and motion vectors: the dilated depth and vectors and the
  previous frame's nearest depth reconstructed;
- Frame interpolation: setup (clears the vector fields and the disocclusion mask, counts frames since the last reset),
  then, but on a reset, the interpolated frame's depth reconstructed, the game's vectors and the optical flow's as
  fields at the interpolated frame, the game field's pyramid (to fill its holes) and the disocclusion mask; the
  interpolation itself, the colour pyramid of its output, and the inpainting of the holes, which also puts the HUD of
  the current picture back where the two pictures differ;
- The copy of the picture without the HUD into the one the next frame interpolates from.

A global compute barrier separates the passes. The interpolation is at the middle, so one generated frame per rendered
frame (2x). After a reset the passes run as AMD's do, priming the histories, and no frame is shown.

The context (the images) is made for the swapchain's size and format and for render sizes up to the larger of that size
and the game's picture (`runtime/patches/widescreen.c` `wide_picture()`), as `runtime/vk/vk_fg.cpp` makes AMD's; another
size or format makes it again after a wait for the device. A context that cannot be made, or passes that cannot be made,
turn frame generation off for the run (`vk_fsr3fg_failed()`), with a line in the log; the frames then go out the plain
way.

The constants live in a host-visible ring of `RING` slots, one per call: a call's slot is read by the present it was
recorded into, and `runtime/gfx/gfx_present.c` waits for the present `FRAMES` (2) back before it records, so a slot
`RING` calls old is done. The same ring index picks the call's timestamp queries, read back when the slot comes round
again.

### The images of a context

- `OF_LEVELS` (7): the optical flow's pyramid levels.
- `OF_BLOCK` (8): pixels per side of its blocks at the finest level.
- `PYR_VIEWS` (13): the inpainting pyramid passes write mips 0-12 through views of their own.
- `SCD_BINS` (256 * 9 = 2304): the scene change histograms: 256 bins in each of 3 x 3 parts of the picture.
- `I_DDEPTH` to `I_DISOCC`: render size (the largest).
- `I_PREV_SRC`: display size, the swapchain's format.
- `I_PYR`: half the display size, every mip.
- `I_LUMA_A`, `I_LUMA_B`: luma pyramids, two frames, `OF_LEVELS` images each.
- `I_FLOW_A`, `I_FLOW_B`: flow pyramids, ping-pong.
- `I_OFVEC`: the finest flow, for interpolation.

`ImgDesc`: `fmt` `VK_FORMAT_UNDEFINED` is the swapchain's; `level` is the pyramid level for `SZ_LUMA` and `SZ_FLOW`; `w`
is the width for `SZ_FIXED` (height 1).

### `img_desc()`

- `I_DIST` is 1x1 R8G8: no distortion, a vector of 0.
- `I_ZERO_U` is 1x1 R32UI: the confidence the passes ignore.

### What the passes bind

The binding numbers of `runtime/shaders/fsr3fi_*.comp` and `runtime/shaders/fsr3of_*.comp`.

`S_*` are sampled images, `U_*` storage images, `B_*` storage buffers (the counters), `C_*` uniform buffers.

`FiConsts`: the constants of frame interpolation (std140 `cbFI`: 4-byte members, the vec4 and the vec3 at 16-byte
boundaries).

The blocks of one ring slot: the interpolation's, the two pyramids', the optical flow's base and one per level, its
SPD's.

`TS_PER` (3) timestamps per call: start, after the optical flow, end.

`PT_MAX`: `NX2SARIA_FSR3FG_PASS_TIMES=1` (diagnosis): a timestamp after every dispatch too, summed by pass
(`vk_fsr3fg_pass_ms()`).

`Ctx`:

- `w`, `h`, `mw`, `mh`: the display size (the swapchain's) and the largest render size.
- `fmt`: the swapchain's format.
- `cnt`: the counters (two words).
- `fresh`: never recorded into: transition and clear everything first.
- `of_odd`: which luma pyramid is the current frame's.
- `of_frame`: optical flow frames since its last reset.
- `have_prev`: `I_PREV_SRC` holds the last frame's picture.

`G`:

- `state`: -1: not tried; 0: off; 1: ready.
- `block`: one constant block (the uniform buffer offset alignment).
- `made_ns`: when the last generated frame was recorded (`vk_fsr3fg_generating()`).
- `pt`: the per-dispatch timestamps (`NX2SARIA_FSR3FG_PASS_TIMES`).
- `pt_pass`: the pass of each dispatch of a slot's call.
- `pt_n`: its dispatches (0: nothing to read).
- `NX2SARIA_FSR3FG_DUMP=<dir>` (diagnosis): every `NX2SARIA_FSR3FG_DUMP_EVERY` seconds (10) the picture before
  (without the HUD), the generated frame and the picture after (with it) are copied into dump, and written as PPM images
  once the slot comes round again (the GPU is done with it then); at most 20 dumps a run.

### `unsupported()`

What the device lacks for the passes, or NULL.

### `make_pass()`

- `s_LinearClamp` of the interpolation's headers, fixed in the layout: nothing to push for it.

### `make_global()`

- A constant block is at least 256 bytes: the largest (`cbFI`, 176 bytes) fits.
- GPU timestamps, where the graphics queue has them.

### `make_ctx()`

- A context half made is freed at once: no submission names it.
- The passes name more mips than a small pyramid has: the views past its last mip name that last one.

### `ctx_for()`

The context for this display size and format, with render sizes up to the larger of rw x rh and the picture's.

- Another size: what a present in flight records into goes first (a wait for the device).
- `NX2SARIA_FSR3FG_TEST_FAIL=1` (test, live): the images are not made, as when video memory runs out.
- Another try after the GPU is idle (images it still held may have gone), then off.

### `vk_fsr3fg_release()`

- The state goes back to "not tried": a new device makes everything again.

### `spd_setup()`

SPD's work groups (64 x 64 pixels each) and mips over w x h, as AMD's `ffxSpdSetup` gives them (mips < 0: from the
size).

- The mip count from the size is floor(log2(max(w, h))).

### `dispatch()`

`Rec`, what one call's dispatches share:

- `VkImageView v[U_FIRST + (B_FIRST - U_FIRST)]`: the image of each slot this dispatch.
- `base`: this call's slot of the ring.
- `k_of`: which optical flow block `C_OF` names (`K_OF` or a level's).
- `k_pyr`: which pyramid block `C_PYR` names.
- `n`: dispatches so far.

### `pt_collect()`

The per-dispatch timestamps of the call RING calls ago in this slot (`NX2SARIA_FSR3FG_PASS_TIMES`).

### `dump_collect()`

Writes the dump the slot holds, if any (`NX2SARIA_FSR3FG_DUMP`): its three pictures as PPM images.

### `dump_record()`

Records this call's dump copies when one is due (`NX2SARIA_FSR3FG_DUMP`, 8-bit pictures only).

### `stats_frame()`

`S`: Each frame by the game's state for the periodic line (`runtime/vk/vk_fg_stats.h`), the one the Windows paths write
too: counted on the present path (`runtime/gfx/gfx_present.c` `ng_record()`) as `vk_fsr3fg_evaluate()` and
`vk_fsr3fg_paused()` see the frames.

- `S_presents`: the presenter's presents so far (`vk_fsr3fg_presents()`).

### `ts_collect()`

The timestamps of the call RING calls ago in this slot, if the GPU wrote them: the averages of the stats.

- Each query gives its value and its availability.
- A time over 1000 ms is a wrapped counter and is not counted.

### `vk_fsr3fg_record()`

- A picture without the HUD of another size or format than bb is not used: the interpolation compares the two pixel by
  pixel.
- The picture's rectangle, inside the image.
- The ring slot: its timestamps of RING calls ago first.
- What drew the inputs and the picture, and the last call's reads of what this one writes, come first.
- A fresh context: every image cleared once, so nothing reads what memory held before.
- The optical flow starts again with the interpolation: its pictures, histograms and scene change result cleared.
- The reconstructed depths start at the far value (the bits of 1.0f; depth not inverted).
- The constants: view depth from device depth (not inverted, finite far plane), in metres, and the projection's scales.
- `tan_half_fov` is the tan of half the horizontal angle; `transfer_function` 0 is sRGB-encoded SDR; `mv_scale` takes
  render pixels to UV.
- The optical flow runs on the pictures without the HUD; the luma pyramids alternate between this frame's and the last
  one's.
- Last, the copy into the picture the next frame interpolates from; with a dump due, the dump read it first.
- The presenter's copy of out, and the next present's writes of bb, come after these.

### `vk_fsr3fg_evaluate()`

- A frame without its inputs: the next frame has no valid frame before it, so it starts a new history.
- These passes generate without the picture without the HUD too (bb stands in, counted in `vk_fsr3fg_stats()`): a frame
  that made none started a history, or failed as logged (counted as one without its inputs).

## `runtime/vk/fsr3_probe.c`

`--probe-fsr3` (`tests/fsr3_check.py` runs it): AMD FSR 3.1 upscaling with the runtime's passes (`runtime/vk/vk_fsr3.c`)
on a synthetic scene, no game files. The scene is a zone plate (rings whose frequency rises with the radius, up to the
render size's limit at its edge) with thin bars and a bright HDR patch, panning at a constant speed; each frame renders
it on the CPU at the render size with the frame's jitter (Halton 2, 3 over FSR's phase count), with a flat depth and the
motion vectors of the pan, and the passes upscale it, 960x540 to 1920x1080.\n\nAfter 48 frames the output is read back
and compared with the scene rendered at the output size (4x4 supersampled, no jitter), as PSNR over the rings
(`rings_mask()`) and over the whole picture, against a bilinear enlargement of the same frame rendered without jitter.
\n\nRuns: the pan; the pan with the jitter's sign reversed and with the motion vectors' sign reversed (the conventions
`runtime/vk/vk_dlaa.c` hands over: a pixel samples the scene at its centre minus the jitter, motion goes from the
current frame to the previous one); the pan sharpened; the scene still, plain and sharpened; then the scene still at
1280x720 on the same output (`other_size()`), on a context of its own, and with the game's dynamic resolution on
(`NX2SARIA_DYN_RES`) on the context made for 960x540. \n\nChecks: no NaN or infinity; on the rings FSR 3.1 beats
bilinear by 0.5 `dB` panning, sharpened FSR 3.1 by 0.5 `dB` still and plain FSR 3.1 is within 0.5 `dB` of it still (at
both render sizes); the reversed jitter does worse and the reversed motion much worse; the second render size makes a
context of its own, and none with dynamic resolution, where its picture is within 3 of 255 of the one of its own
context; no validation error from the passes (run with `NX2SARIA_VK_VALIDATION=1`). \n\nThen the GPU time of one upscale
at the docked sizes (1707x960 -> 2560x1440, the Quality mode) with and without sharpening, and outside Windows the
upscaler switched off: `vk_ffx_release_idle()` leaves no context, and the next upscale makes one.\n\nThe pan, (1, 0.5)
render pixels a frame, moves the picture by whole output pixels; at most other speeds (`NX2SARIA_FSR3_PROBE_PAN=vx,vy`)
FSR 3.1's picture of these rings comes out softer, below bilinear, and on a pan slower than about 0.2 render pixels a
frame it runs about a third of a render pixel ahead of the scene. `amd_fidelityfx_vk.dll`, given the same inputs, does
the same: its output and the passes' are within 3 of 255 (level) for the pan of (1, 0.5) and the still scene at 960x540,
the still scene at 1280x720 on a context of its own and on one for every size up to the output's, and a pan of (1.33,
0.67) render pixels at 960x540, where both score about 28 `dB` on the rings, 7 `dB` below the pan of (1, 0.5).
\n\n`NX2SARIA_FSR3_PROBE_PPM=<dir>` writes the outputs, the bilinear ones and the references as PPM images.

The scene is in render pixels of the 960x540 render; S is how many output pixels make one of them.

### `scene()`

- The zone plate is centred at (480, 270) with k = pi / 2400: at r = 600 the rings reach 1 per pixel, and beyond it is a
  flat field of 0.5.
- Thin dark bars, 0.6 px wide, every 37 pixels across and every 53 down.
- A bright patch of HDR values around (140, 120).

### `record()`

- The first frame takes the images into GENERAL.
- A camera like the game's in Hyrule Field (`runtime/vk/vk_dlaa.c` `proj_planes()`): near 0.1, far 25,000, in metres.

### `fill_inputs()`

The inputs of frame i of a run: the scene at time i, sampled at each pixel's centre minus the jitter (sign: the jitter's
sign as handed to the passes, -1 reversed); motion from this frame to the previous one is minus the pan.

### `level()`

The 8-bit level of a picture's value in the PPM images (and in the comparison of two pictures, `other_size()`).

- A soft roll-off (v / (1 + v) * 2) so the HDR patch stays visible.

### `psnr()`

PSNR over [0, 1] (the HDR patch clamped) of a against the reference, over the pixels where mask is set (NULL: all).

### `rings_mask()`

The rings alone at time t: output pixels at least 2.5 render pixels from a bar, away from the HDR patch and the image's
border. The bars are 0.6 render pixels wide: thinner than any reconstruction from render-size samples can keep at full
contrast, so they weigh on the whole picture's PSNR the same for every upscaler (and favour the bilinear one, whose
unjittered samples sit on them).

### `run()`

One run of n frames; fills `out[]` with the last output (luma = the red channel, the scene is grey). Returns 0 when a
frame failed or the output holds a NaN or an infinity.

### `reference()`

The reference for time t (the scene, 4x4 samples per output pixel) and the bilinear enlargement of the frame at t
rendered without jitter (lr: render-size scratch).

### `other_size()`

Another render size on the same output (w x h -> ow x oh), the scene still, as the menu switches it while the game runs.
Without dynamic resolution it gets a context of its own (`runtime/vk/vk_fsr3.c` `ctx_for()`: one per render size); with
it, the context made for the first size (`*first`, which has run before) serves it, and its picture must be the one its
own context gives. `*q` and `*q_bil`: PSNR on the rings of FSR 3.1 (its own context) and of bilinear; `*made` and
`*made_drs`: the contexts made for it without and with dynamic resolution; `*diff`: the largest difference between the
two pictures, in 8-bit levels (level). 0 when a test image could not be made or a frame failed. `NX2SARIA_DYN_RES` is 0
afterwards.

- With dynamic resolution: one context made anew at the first size (the GPU is idle after each frame).

### `probe_fsr3()`

- What an older validation layer reports about the device's creation (structures newer than it) is not ours.
- 960x540 to 1920x1080 is the Performance mode (2x).
- The Quality mode's render size on the same output, as the menu switches it while the game runs.
- A context for render sizes up to the output's gives FSR 3.1 larger images than the render size, and its picture moves
  by a few levels: `amd_fidelityfx_vk.dll`'s two pictures of this run, with and without
  `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION` and its larger `maxRenderSize`, differ by up to 3.
- GPU time at the docked sizes, the inputs uploaded once.
- The upscaler switched off (`runtime/vk/vk_dlaa.c` `retire_idle()`, the GPU idle after each frame here):
  `vk_ffx_release_idle()` lets the contexts go, `vk_upscaler_let_go()` destroys them (the GPU is past them), and the
  next upscale makes one again.
- What the device's destruction reports is about objects other parts of the runtime made, not counted either.

## `runtime/vk/fsr3fg_probe.c`

`--probe-fsr3fg` (`tests/fsr3fg_check.py` runs it): AMD FSR 3.1 frame generation with the runtime's passes
(`runtime/vk/vk_fsr3fg.c`) on a synthetic scene, no game files and no window. The picture (1280x720, RGBA8 like an SDR
swapchain image) is a textured background (blocks of noise over a gradient, which the optical flow can follow) with a
yellow square moving right by 16 pixels a frame, a HUD bar (a green rectangle) drawn over it in the picture with the HUD
and left out of the picture without it, black bars above and below the picture's rectangle; the depth (D32F, 0 near) and
the motion vectors (R16G16F, render pixels, current to previous) are at half the size, 640x360, the square nearer than
the background. Each frame is uploaded and generated as `runtime/gfx/gfx_present.c`'s presenter records it; after 12
frames the generated frame (between the last two) is read back.

Checks: the first frame and a reset show no generated frame and every other does; the square's centre in the generated
frame is within 2 pixels of halfway between the two frames' (not at either end); the HUD bar is the picture with the
HUD's to within 2 of 255; the black bars stay black; no validation error from the passes (with
`NX2SARIA_VK_VALIDATION=1`). The same run with the motion vectors' sign reversed is reported (the optical flow alone may
still place the square). Then the GPU time of one generated frame at 2560x1080 from 1920x810 (the 21:9 1080p of the
bench) and at 1920x1080 from 1280x720, from the passes' own timestamps, and `vk_fsr3fg_idle()` freeing the images.
`NX2SARIA_FSR3FG_PROBE_PPM=<dir>` writes the last two pictures and the generated one as PPM images.

The square's side `SQ` is 64 picture pixels; black bars of `BAR_TOP` (60) pixels are above and below the picture's
rectangle.

### `fill()`

Frame i's pictures (with and without the HUD), depth and motion vectors; mvsign -1 reverses the vectors.

- The background: 8x8 blocks of noise over a gradient: blue-grey, never the square's or the HUD's colour.
- Current to previous: the square was vx picture pixels to the left, vx / sx render pixels.

### `square_x()`

The square's centre (x) and its pixel count in an RGBA8 picture.

### `Result`

- `shown_ok`: the first frame and the reset showed nothing, every other frame showed one.
- `cx`, `expect`: the square's centre in the generated frame and halfway.
- `px`: its pixels there.
- `hud_diff`, `bars`: the largest HUD difference; bar pixels that are not black.

### `run()`

One run of `frames` frames at W x H from rw x rh, panning vx picture pixels a frame; the generated frame between the
last two is read back. A reset at frame `reset_at`.

- A new run starts a new history (`vk_fsr3fg_reset()`).
- The HUD: as in the picture with it; the bars: black.

### `probe_fsr3fg()`

- GPU time at the bench's 21:9 1080p and at 1080p, from the passes' timestamps.
- A call's timestamps are read RING (8) calls later: a first run reads the last run's and warms this size up, and the
  second one's 40 frames read 40 timed frames of this size.
- With `NX2SARIA_FSR3FG_PASS_TIMES=1`: each pass's share.
- Last, as it is for the rest of a run (`NX2SARIA_FSR3FG_TEST_FAIL`): images that cannot be made turn frame generation
  off, nothing worse.

## `runtime/vk/vk_ffx.h`

What `runtime/vk/vk_ffx.cpp` and `runtime/vk/vk_ffx4.cpp` promise their callers, and the declarations of what
`runtime/vk/vk_dlaa.c` gives every upscaler.

`VkFfxImage`: `image` is level 0, layer 0, in `VK_IMAGE_LAYOUT_GENERAL`; `storage` says it was created with
`VK_IMAGE_USAGE_STORAGE_BIT` (FSR writes it); `view` is a 2D view of level 0, layer 0 (the depth aspect for depth),
which `runtime/vk/vk_fsr3.c` binds.

`VkFfxCamera`: the scene camera FSR takes, from the frame's projection (`runtime/vk/vk_dlaa.c` `proj_planes()`): the
vertical field of view in radians, the near and far planes' view distances (far: `VK_DLAA_FAR_INFINITE`, 1e8, for a
projection without a far plane) and the metres in one unit of view space. A field of 0 is not known, and FSR then gets 1
radian, 0.1, 1e8 and 1.

From `runtime/vk/vk_dlaa.c`, on the thread that makes the upscalers' calls (its pass hooks):

- `vk_dlaa_fsr3_boost()`: the share (0..1) of the extra sharpening FSR 3.1 gets at a render width w of an output width
  ow.
- `vk_upscaler_device_idle()`: a wait for the device to be idle, the queue lock held, before an upscaler frees what the
  GPU may still use (`NX2SARIA_TRACE=perf`: "upscaler GPU waits", with those of `vk_upscaler_gpu_wait()`).
- `vk_upscaler_idle_wait()`: `NX2SARIA_UPSCALER_IDLE_WAIT`: 1 waits for the device wherever an upscaler lets go of
  something.
- `vk_upscaler_let_go()`: after something was let go of: what the GPU is past goes (with `NX2SARIA_UPSCALER_IDLE_WAIT=1`
  after a wait for the device, `vk_upscaler_settle()`).
- `vk_upscaler_settle()`: a wait for the device, then everything let go of that a submission made so far names goes (a
  present gate stays).
- `vk_upscaler_gpu_wait()`: a refused creation: waits, without the device's idle, until the GPU is past timeline value v
  (no further than the submissions made so far) and returns the value it is past; with `NX2SARIA_UPSCALER_IDLE_WAIT=1`,
  or when that wait fails, a wait for the device.

FSR 3.1:

- `vk_ffx_load()`: 1 when FSR 3.1 upscaling can run: on Windows the library is there with its entry points (loaded
  once); elsewhere the runtime's passes are made (`runtime/vk/vk_fsr3.c` `vk_fsr3_init()`: they need the device, so 0
  before it exists, and asked again then).
- `vk_ffx_available()`: whether FSR 3.1 upscaling can run in this build, for the settings menu: after `vk_ffx_load()`
  ran, what it found; before, on Windows whether the library file is there, asked once without loading it, elsewhere
  whether the runtime's passes may run (`vk_fsr3_available()`).
- `vk_ffx_framegen_available()`: the same for frame generation.
- `vk_ffx_upscale()`: records FSR 3.1 into cb: color (the scene light, render size), depth (D32F), mv (R16G16F, render
  pixels, current to previous), out (the output size, RGBA16F, storage). jx, jy: the jitter the scene took, render
  pixels. cam: the frame's camera. 0 on failure (the caller falls back as it does when DLSS fails).
- `vk_ffx_release()`: before the device goes away (the device idle).
- `vk_ffx_holds()`, `vk_ffx_release_idle()`: the upscale contexts of FSR 3.1 and FSR 4 (`runtime/vk/vk_dlaa.c`
  `retire_idle()`): whether any is held, and all of them let go of (the caller's `vk_upscaler_let_go()` frees them once
  the GPU is past them).
- `vk_ffx_trim_idle()`: FSR 3.1's contexts that no upscale used in the last `recent` calls, let go of: room for an FSR 4
  context whose creation failed, whose caller makes their memory free (`vk_ffx_dead_take()` after
  `vk_upscaler_gpu_wait()`). Returns their count.
- `vk_ffx_dead_take()`, `vk_ffx_dead_fence()`: the FSR 3.1 and FSR 4 contexts let go of (outside Windows FSR 3.1's are
  `runtime/vk/vk_fsr3.c`'s): those whose last upscale's submission is at most `done` are destroyed (their count); the
  highest such submission among those left (0: none).
- `vk_ffx_proc_addr()`: the device's functions as the library is given them.
- `vk_ffx_create()`, `vk_ffx_destroy()`, `vk_ffx_configure()`, `vk_ffx_query()`, `vk_ffx_dispatch()`: the API's calls
  themselves, for `runtime/vk/vk_fg.cpp` (0 = `FFX_API_RETURN_OK`); the library is loaded first (`vk_ffx_load()`). C++
  only, and not declared with `VK_FFX_NO_API`.

FSR 4 (`runtime/vk/vk_ffx4.cpp`), which `vk_ffx_upscale()` runs in FSR 3.1's place when the setting asks
(`NX2SARIA_FSR4`) and it runs here, falling back to FSR 3.1 when it fails:

- `vk_ffx4_available()`: the libraries are there and list a 4.x version for the device; asked once, with the device,
  from any thread (0 before the device exists).
- `vk_ffx4_name()`: "AMD FSR 4.1.2" once `vk_ffx4_available()` found it.
- `vk_ffx4_upscale()`: as `vk_ffx_upscale()`.
- `vk_ffx4_holds()`: an FSR 4 upscale context is made. `vk_ffx4_release_idle()`: every one let go of.
  `vk_ffx4_dead_take()`, `vk_ffx4_dead_fence()`: FSR 4's part of `vk_ffx_dead_take()` and `vk_ffx_dead_fence()`.
- `vk_ffx4_failed()`: it failed `FAIL_MAX` times in a row: FSR 3.1 serves until `vk_ffx4_retry()`, which the menu calls
  when FSR 4 is chosen again.
- `vk_ffx4_trim()`: at each upscale: FSR 4's contexts unused for a while go.
- `vk_ffx4_vram()`: FSR 4's upscale contexts and their video memory (`vk_ffx_vram()`).
- `vk_ffx_fsr4()`: the setting asks for FSR 4 and it runs here: what `vk_ffx_upscale()` uses, for the menu.

## `runtime/vk/vk_ffx.cpp`

AMD FidelityFX through its API library (`amd_fidelityfx_vk.dll` next to the executable, loaded at run time): FSR 3.1
temporal upscaling here, in the place of DLSS in `runtime/vk/vk_dlaa.c`, and the API's calls for frame generation
(`runtime/vk/vk_fg.cpp`). The API's headers (`runtime/third_party/ffx_api/`) are C++ in places, hence this file. Nothing
of it runs unless the upscaler or frame generation asks for FSR 3.1.

AMD publishes that library for Windows only. The other builds upscale with the runtime's own build of FSR 3.1's passes
(`runtime/vk/vk_fsr3.c`), which takes the same inputs, camera and sharpness, and generate frames with the runtime's own
build of FSR 3.1's frame generation passes (`runtime/vk/vk_fsr3fg.c`, through `runtime/gfx/gfx_present.c`'s presenter).

FSR 3.1 takes the same inputs `runtime/vk/vk_dlaa.c` makes for DLSS: the scene's light target (linear HDR), its D32F
depth (0 near, 1 far, with the near and far planes of the frame's own projection, `runtime/vk/vk_dlaa.c`
`proj_planes()`) and the camera motion vectors in render pixels from the current frame to the previous one, the
convention both share, so the vectors go in unscaled.

`g_loaded` and `g_present` hold what the settings menu may ask before anything has used FSR 3.1 (`vk_ffx_available()`),
from any thread: what `vk_ffx_load()` found, and whether the library file is there (1 or 0; -1 while not asked). The
library is `amd_fidelityfx_vk.dll` next to the executable (`library_path()`).

Outside Windows `vk_ffx_load()` is the runtime's passes, made once the device exists (0 before, and asked again then).

### `compute_pipelines()`, `graphics_pipelines()`

The library's pipeline creates (`vk_ffx_proc_addr()`). It passes no pipeline cache, so a create that names none goes
through the runtime's (`runtime/vk/vk_pipeline.c` `vk_pipeline_create_foreign()`); one that names a cache is the
library's own business. `pipe_cache_on()` reads `NX2SARIA_FFX_PIPE_CACHE` once.

### `create_counted()`

`ffxCreateContext` with what it made in pipelines: the library compiles every pass of a context inside this call, on the
thread that asked, so the line says what a context cost and where its pipelines came from.

### `context_for()`

One upscaling context per render size and output size, as AMD's own sample keeps them: it waits for the GPU and makes
the context anew whenever the render resolution changes, and sets `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION` only for its
dynamic resolution mode. One context per output size that every mode of the menu ran on, with another render size at
each dispatch and without that flag, preceded an access violation in AMD's driver inside FSR 4's dispatch at the first
frame after a switch from 67 % to 100 % (`runtime/vk/vk_ffx4.cpp` keeps its contexts the same way).

With the game's dynamic resolution on (`NX2SARIA_DYN_RES`, `runtime/pacing/dynres.c`), whose steps change the render
size every few seconds, one context with that flag takes them all: it is made for every size up to the output's (and a
scene at the output size).

A context of another render size goes once it is not among the recent ones (`UP_RECENT`), so a switch keeps at most two
of an output size for a few frames, and the game's second scenes, of another output size, keep their own. When all slots
are taken, the least recently used one goes for the new size. The work of a context that goes may still be in flight: it
is let go of (`retire_up()`) and destroyed once the GPU is past the submission of its last upscale (`g_dead`), without a
wait for the device.

- No infinite-depth flag: the game's far plane is finite (`runtime/vk/vk_dlaa.c` `proj_planes()`) and each dispatch
  gives it.
- `NX2SARIA_FFX_DEBUG=1` (test): the library checks the values of each dispatch and says what it finds (`message()`).
- A creation refused while contexts let go of (this file's and FSR 4's) are still in flight: most often their video
  memory, which a wait for the device freed before the creation up to 0.10.1. The GPU finishes with them
  (`vk_upscaler_gpu_wait()`: those submissions only, not the device), they go, and the creation is tried once more, as
  `runtime/vk/vk_ffx4.cpp` `context_for()` and `runtime/vk/vk_dlaa.c` `evaluate()` do, rather than the upscaler failing
  for the run.
- The context's video memory is asked for the memory report (`vk_ffx_vram()`); a provider that does not know the query
  leaves it 0.

`Up`, one context:

- `mw`, `mh`, `ow`, `oh`: the render size it was made for (the largest it takes with `drs`); the output size.
- `drs`: made for dynamic resolution (`FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION`).
- `used`, `used_ns`: `up_tick` and the time when `context_for()` last returned it.
- `vram`: its video memory as the library reported it at its creation, 0 not told.
- `fence`: the submission of its last upscale (`vk_exec_recording_fence()`), 0 none.

`UP_RECENT` (8): the contexts used in the last few calls stay: a frame makes one or two calls (the scene, and a second
scene after it), so this is a few frames back. That is the policy only: a context that goes waits for the GPU through
its fence, also when the command buffer that names it is not submitted yet.

`UP_IDLE_NS` (90 seconds): a context no upscale used for this long goes (`trim()`): FSR 3.1's after a switch to FSR 4,
and a second scene's (the pause screen's) a while after it closed.

### `retire_up()`, `dead_release()`

`g_dead`: the contexts let go of, destroyed once the GPU is past their last upscale (`vk_ffx_dead_take()`). Only the
thread that makes the upscales (`runtime/vk/vk_dlaa.c`'s pass hooks) touches it; `vk_ffx_release()` at teardown.
`dead_release()` is timed as "upscaler releases" (`NX2SARIA_TRACE=perf`).

The list full, which the GPU's progress does not let happen: room after a wait for the device, or else all of them go as
before this list existed, the device idle.

### `image()`

Windows only, for the library's dispatch (`vk_ffx_upscale()`); the other builds hand `runtime/vk/vk_fsr3.c` the images
themselves.

### `configure_keys()`

`NX2SARIA_FFX_KEYS=<key>:<value>[,...]` (test): FSR 3.1's tuning constants for each new context
(`FfxApiConfigureUpscaleKey` of the API's `ffx_upscale.h`: 0 velocity factor, 1 reactiveness scale, 2 shading change
scale, 3 accumulation added per frame, 4 minimum disocclusion accumulation). The library reads the value through the
pointer.

### `vk_ffx_available()`

The menu asks every frame: on Windows it looks for the file once, and leaves the loading to the first use. Elsewhere the
answer is the runtime's passes': 1 until the device shows it cannot run them.

### `vk_ffx_framegen_available()`

Frame generation on Windows runs through the library's own API calls (`vk_ffx_create()` and the rest), so only where the
library loads, and it needs a queue for the frame generation swapchain's presents (`vk_fg_has_queues()`). Elsewhere it
runs through the runtime's passes and `runtime/gfx/gfx_present.c`'s presenter, wherever the device runs them.

The answer there is `runtime/vk/vk_fsr3fg.c`'s: 1 until the device shows it cannot run the passes.

### `vk_ffx_fsr4()`

`NX2SARIA_FSR4` (default 0, live): FSR 4 where it runs (`runtime/vk/vk_ffx4.cpp`) and has not failed in a row, else FSR
3.1.

### `vk_ffx_dead_take()`

Outside Windows it also takes the runtime's passes' contexts (`runtime/vk/vk_fsr3.c` `vk_fsr3_dead_take()`), FSR 3.1's
there.

### `vk_ffx_upscale()`

- Each upscaler keeps a history of its own: the frame after a change from one to the other starts it anew, or it would
  blend this picture with whatever that upscaler last saw.
- `last` (3 or 4) is which one made the frame before.
- When FSR 4 fails it says so, and FSR 3.1 makes this frame.
- The sharpness slider (0-100) drives FSR's own RCAS, as it does for FSR 1. Below a render scale of three quarters it
  goes towards the maximum, all the way from Performance down, where FSR 3.1 is soft (`runtime/vk/vk_dlaa.c`
  `vk_dlaa_fsr3_boost()`, which has the measurements); 0 still turns it off.
- Outside Windows the call goes to `runtime/vk/vk_fsr3.c` `vk_fsr3_upscale()` from here. On Windows: every image is in
  GENERAL, which the backend reads as its unordered-access state, and is left there.
- The frame's camera (`runtime/vk/vk_dlaa.c` `proj_planes()`).
- The context's fence is the submission being recorded, taken after the dispatch: it was recorded into cb (a refusal
  too, for all this side knows).

### `vk_ffx_create()`

The library's create, counted (`create_counted()`): never `runtime/vk/vk_fsr3.c`.

### `vk_ffx_proc_addr()`

What the library is handed as `ffxCreateBackendVKDesc.vkDeviceProcAddr`: the device's functions, with the two pipeline
creates replaced by ours (`compute_pipelines`, `graphics_pipelines`) unless `NX2SARIA_FFX_PIPE_CACHE` is 0.

### `vk_ffx_probe_cache()`

`--probe-ffx-cache` (`runtime/host.h`): one FSR 3.1 upscaling context on the device, without a window or the game, and
where its pipelines came from. The pipeline caches are those beside the executable, or `NX2SARIA_PIPELINE_CACHE`'s, and
are saved as after a run, so a second run of the probe shows what the first one's files give (`tests/ffx_cache_check.py`
runs it twice on a folder of its own). Fails when the context is not created, or when the library made no pipeline
through the runtime's creates although they are on.

- The first `vk_pipeline_*` call (`vk_pipeline_push_fn()`) reads the pipeline caches.

### `vk_ffx_holds()`

The upscaler is off (`runtime/vk/vk_dlaa.c` `retire_idle()`): FSR 3.1's and FSR 4's upscale contexts are let go of, made
again at the next upscale; outside Windows FSR 3.1's are `runtime/vk/vk_fsr3.c`'s. The caller's `vk_upscaler_let_go()`
frees them once the GPU is past them. Frame generation's contexts are not these.

### `vk_ffx_release()`

Teardown and the probe: the device is idle.

### `vk_ffx_vram()`

The memory report's FSR figures (`runtime/vk/vk_memory.c` `outside_log()`): the video memory of the upscaling contexts
that exist, FSR 3.1's here and FSR 4's (`vk_ffx4_vram()`), as the library reported each at its creation, with their
count; and frame generation's (`vk_fg_vram()`). The library allocates it itself, outside VMA. Read without a lock: a
context made or destroyed meanwhile shows in the next report.

## `runtime/vk/vk_ffx4_api.h`

The entry points of AMD's FSR SDK 2.4 loader (`amd_fidelityfx_loader_vk.dll`, which `runtime/vk/vk_ffx4.cpp` loads), for
`runtime/vk/vk_fg4.cpp`. C++ and Windows only, and never in one file with the API declarations of `runtime/vk/vk_ffx.h`:
the two sets of headers name the same types with other layouts.

- `vk_ffx4_api_versions()`: the versions the loader lists for one kind of context (a
  `FFX_API_CREATE_CONTEXT_DESC_TYPE_*`) on this device, in its order, newest first: their count, at most cap. The names
  are the library's memory. 0 without the loader, before the device exists, or when it lists none (an effect whose
  library is not beside the executable).

- `vk_ffx4_api_file()`: a file of that name is next to the executable (one of the SDK's libraries).

- `vk_ffx4_api_create()`, `vk_ffx4_api_destroy()`, `vk_ffx4_api_configure()`, `vk_ffx4_api_query()`,
  `vk_ffx4_api_dispatch()`: the API's calls (0 = `FFX_API_RETURN_OK`), valid once `vk_ffx4_api_versions()` returned a
  count.

## `runtime/vk/vk_ffx4.cpp`

AMD FSR 4 (FSR Upscaling 4.x, the machine-learned upscaler) through the libraries of AMD's FSR SDK 2.4:
`amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` next to the executable, loaded at run time. The
SDK's headers (`runtime/third_party/ffx_sdk/`) are at the paths they have in the SDK. FSR 3.1 and frame generation stay
with `runtime/vk/vk_ffx.cpp` and the library of SDK 1.1.4: the two sets of headers name the same types with other
layouts, so nothing of the API crosses between the two files, and `runtime/vk/vk_ffx.cpp` `vk_ffx_upscale()` calls in
here first.

The loader answers which upscaler versions it can run on a device (`ffxQueryDescGetVersions` with the device given):
4.1.2, 3.1.6 and 2.3.4 on a Radeon the ML upscaler supports (RX 6000, 7000 and 9000 cards and some Ryzen APUs, with
AMD's driver 26.10.1 or newer on Windows), 3.1.6 and 2.3.4 on any other. FSR 4 runs only where a 4.x version is listed,
and its contexts are made with that version asked for by its id, so the loader cannot hand out another. Where it is not
listed, or a context or a dispatch fails, `runtime/vk/vk_ffx.cpp`'s FSR 3.1 serves the frame as before.

The inputs are the ones FSR 3.1 takes (`runtime/vk/vk_ffx.cpp`): the scene's light target in linear HDR, its D32F depth
(not inverted, the far plane at infinity) and the camera motion vectors in render pixels from this frame to the previous
one. FSR 4 needs no reactive or transparency mask.

AMD publishes the SDK's libraries for Windows only: in any other build FSR 4 is never listed, `runtime/vk/vk_ffx.cpp`'s
FSR 3.1 path is all there is, and the SDK's headers are not read. The file defines `VK_FFX_NO_API` before it includes
`runtime/vk/vk_ffx.h`, which then leaves out the API declarations of the other set of headers.

The state of the probe:

- `g_lock`: the probe's lock: the menu asks from its thread, the replay from its own.
- `g_state`: -1 not probed; 0 FSR 4 does not run here; 1 it does.
- `g_off`: `FAIL_MAX` (4) failures in a row: FSR 3.1 serves until the menu asks again (`vk_ffx4_retry()`).
- `g_version`: the id of the 4.x version the loader listed for the device.
- `g_name`: "AMD FSR 4.1.2", for the menu and the log.
- `g_failures`: contexts or dispatches that failed in a row.
- `g_retry_ns`, `RETRY_NS` (2 seconds): after a context creation failed, FSR 3.1 serves the frames until the next try
  this much later: a save loading fills the video memory with the area's textures for a few frames, and three tries in
  three frames there turned FSR 4 off.

### `beside_exe()`

A library of the SDK next to the executable; 0 when the path does not fit.

### `load()`

Loads the SDK's loader from beside the executable. Under `g_lock`, once the device exists (-1 before: asked again
later). `g_loader`, under `g_lock`: -1 not tried, 0 the loader is not there or does not load, 1 its entry points are in
`A`. A loader not shipped beside this build is not logged: FSR 3.1 is what the menu offers.

### `versions()`

The versions the loader lists for one kind of context on this device, in its order: their count, at most cap. Under
`g_lock`, the loader loaded. The first query asks for the count.

### `probe()`

Asks the loader for the device's upscaler versions. Under `g_lock`; -1 before the device exists. A version's name is
"4.1.2", or "4.1.2 \*" for a version the driver brings; the name is the library's memory, so it is copied, without the
driver's mark.

### `create()`

A context in `slot` for render sizes up to mw x mh and the output size ow x oh; NULL when it is not made.

One context per render size and output size, kept as `runtime/vk/vk_ffx.cpp` `context_for()` keeps FSR 3.1's: a new
render size makes a context of its own (50 to 70 ms and up to about 217 MiB of video memory) and the one before goes a
few frames later, so a mode change costs a creation; with the game's dynamic resolution on one context with
`FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION` takes every step. Two other ways were tried:

- Contexts kept for every mode tried piled up to six beside FSR 3.1's, and three creations that failed (the card out of
  memory) turned FSR 4 off for the run.
- One context for every render size preceded an access violation in AMD's driver inside the dispatch after a switch from
  67 % to 100 %.

The descriptors a context was made from stay with it, as the API asks ("must remain live until ffxDestroyContext"): in a
`Made` of its own on the heap (zeroed, as the descriptors' `{}` were), so that a context let go of (`retire_up()`) keeps
them until it is destroyed, once the GPU is past its last upscale (`g_dead`), while its slot already holds the next one.

- The flags are HDR and auto exposure, with a finite far plane, as FSR 3.1's.
- `NX2SARIA_FFX_DEBUG=1` (test): the library checks the values of each dispatch and says what it finds (`message()`).
- The provider is asked for the version it runs: anything but a 4.x here means the loader gave another upscaler, and the
  context, which the GPU never used, is destroyed at once.

`Up`, one context: as in `runtime/vk/vk_ffx.cpp`, with `made`, the descriptors it was made from (calloc), freed after
its destroy.

`UP_RECENT` (8): contexts used this recently stay (as `runtime/vk/vk_ffx.cpp`'s): the policy only, as one that goes
waits for the GPU through its fence. `UP_IDLE_NS` (90 seconds): a context no upscale used for this long goes
(`vk_ffx4_trim()`): FSR 4's after a switch to FSR 3.1, and a second scene's (the pause screen's) a while after it
closed.

### `retire_up()`, `dead_release()`

`g_dead`: the contexts let go of, destroyed once the GPU is past their last upscale (`vk_ffx4_dead_take()`): `handle` is
the context, `aux` its `Made`. Only the thread that makes the upscales (`runtime/vk/vk_dlaa.c`'s pass hooks) touches it;
`vk_ffx4_release()` at teardown. `dead_release()` is timed as "upscaler releases" (`NX2SARIA_TRACE=perf`).

The list full, which the GPU's progress does not let happen: room after a wait for the device, or else all of them go as
before this list existed, the device idle.

### `context_for()`

The context for a render size and an output size: the one made for them, else a new one, in a free slot or in place of
the least recently used one; the others of that output size go once they are not recent. What goes is let go of and
destroyed once the GPU is past it (`retire_up()`): its work may be in flight.

A creation that fails is tried once more after every context not in use has gone, FSR 3.1's idle ones too
(`vk_ffx_trim_idle()`), and the GPU has finished with them and with those let go of before: what it lacked was most
often video memory. The creation needs their memory now, so this is a wait for the GPU to pass them all, this file's and
FSR 3.1's (with `NX2SARIA_UPSCALER_IDLE_WAIT=1` for the device).

### `failed()`

Failures in a row: `FAIL_MAX` of them turn FSR 4 off until the menu asks for it again (`vk_ffx4_retry()`); a success
starts the count again, so a creation that failed once in a while does not add up over a session.

### `vk_ffx4_retry()`

The menu chose FSR 4 again: after failures that turned it off, it is tried anew.

### `vk_ffx4_trim()`

Called at every upscale, with FSR 4 or FSR 3.1 (`runtime/vk/vk_ffx.cpp` `vk_ffx_upscale()`), on the thread that records
them.

### `vk_ffx4_upscale()`

- After a failure FSR 3.1 serves until the next try (`g_retry_ns`).
- Every image is in GENERAL, which the back end reads as its unordered-access state, and is left there.
- The sharpness slider (`NX2SARIA_SHARPNESS`) drives the upscaler's own sharpening, as for FSR 3.1.
- The frame's camera (`runtime/vk/vk_dlaa.c` `proj_planes()`), as FSR 3.1 gets it (`runtime/vk/vk_ffx.cpp`).
- The context's fence is the submission being recorded, taken after the dispatch: it was recorded into cb (a refusal
  too, for all this side knows).

### `vk_ffx4_api_file()`

The loader's calls for `runtime/vk/vk_fg4.cpp` (`runtime/vk/vk_ffx4_api.h`), this one and the `vk_ffx4_api_*` functions
after it.

### `vk_ffx4_release()`

Teardown and the probe: the device is idle.

### `vk_ffx4_release_idle()`

The upscaler is off (`runtime/vk/vk_ffx.cpp` `vk_ffx_release_idle()`): every context is let go of.

### `vk_ffx4_vram()`

FSR 4's upscaling contexts that exist and their video memory, for the memory report (`runtime/vk/vk_ffx.cpp`
`vk_ffx_vram()`).

## `runtime/vk/vk_fg.h`

What `runtime/vk/vk_fg.cpp` promises `runtime/gfx/gfx_present.c`, whose swapchain AMD's frame generation swapchain
replaces.

- `vk_fg_wanted()`: `NX2SARIA_FRAMEGEN` = fsr3 (live), where FSR 3.1 loads, with any upscaler or none.
- `vk_fg_queue_count()`: graphics-family queues the device creates.
- `vk_fg_queues()`: called by `runtime/vk/vk_device.c` after the device exists: the queues AMD's swapchain presents and
  acquires on, and their families (the graphics family's own where it has three queues, else queues of another family:
  AMD's Windows driver gives one).
- `vk_fg_has_queues()`: `vk_fg_queues()` got a present queue: frame generation can run here.
- `vk_fg_active()`: the frame generation swapchain exists. `vk_fg_generating()`: and the last frame had generation on
  (not paused for the menu).
- `vk_fsr3fg_generating()`: FSR 3.1's frame generation through the runtime's passes (`runtime/vk/vk_fsr3fg.c`, every
  build but the Windows one): a generated frame was made in the last 250 ms, so the screen shows two frames for each the
  game draws (`runtime/pacing/frame_rate.c` `generated_factor()`). 0 while off, paused for the menu, or between
  histories for longer than that. Any thread.
- `vk_fg_create_swapchain()`, `vk_fg_destroy_swapchain()`, `vk_fg_get_images()`, `vk_fg_acquire()`, `vk_fg_present()`:
  `runtime/gfx/gfx_present.c`'s swapchain calls while `vk_fg_wanted()`: the first create makes AMD's swapchain context,
  later ones (window resizes) its swapchain again. Non-success from the first: the plain swapchain path stays. The
  present is on the game's queue, its lock held by the caller.
- `vk_fg_frame()`: each present, before its command buffer is submitted: configures generation for this frame and
  records its preparation (the frame's depth and motion vectors, `runtime/vk/vk_dlaa.c` `vk_dlaa_fg_inputs()`) into cb.
  hudless: this frame's picture without the HUD at the swapchain's size and format, in GENERAL (`VK_NULL_HANDLE`: none),
  from which generation tells the HUD apart and keeps it out of the generated frames. pic: the picture's rectangle in
  the swapchain {x, y, w, h} (black bars around it when the window has another shape); frames are generated inside it
  only.
- `vk_fg_presents()`: frames the swapchain presented, generated ones included (0: off).
- `vk_fg_hdr_metadata_fn()`: AMD's replacement for `vkSetHdrMetadataEXT` while active, else NULL.
- `vk_fg_vram()`: the frame generation context's video memory, FSR 3.1's or FSR 4's; 0 none or not told.

The `vk_fg4_*` functions are the same through AMD's FSR SDK 2.4 (`runtime/vk/vk_fg4.cpp`), for `runtime/vk/vk_fg.cpp`
alone: it takes that path while FSR 4 is the upscaler, and every call above goes to these while that path holds the
swapchain.

- `vk_fg4_available()`: its libraries are beside the executable and list a frame generation for the device.
- `vk_fg4_failed()`: it failed in this run: FSR 3.1's library generates the frames.
- `vk_fg4_name()`: the frame generation version the loader lists first for the device ("3.1.6"), "" unknown.

## `runtime/vk/vk_fg.cpp`

Frame generation with AMD FSR 3.1 (`NX2SARIA_FRAMEGEN` = fsr3, live: `runtime/gfx/gfx_present.c`
`window_settings_live()` makes the swapchain again; a run whose device was made for DLSS Frame Generation waits for a
restart, `vk_fg_wanted()`): AMD's frame generation swapchain (`amd_fidelityfx_vk.dll`, which `runtime/vk/vk_ffx.cpp`
loads) takes the place of `runtime/gfx/gfx_present.c`'s swapchain. Every frame the game presents, the swapchain presents
a generated frame between it and the one before, on its own present queue and pacing threads, so the screen shows twice
the game's frame rate.

The generated frame is built from the presented picture and the scene's depth and camera motion vectors, which
`runtime/vk/vk_dlaa.c` makes for DLSS and FSR 3.1 upscaling, and without an upscaler for frame generation alone
(`runtime/vk/vk_dlaa.c` `fg_only_frame()`: the same pass at the scene's size, no jitter). The HUD is kept out through
the picture without it (`runtime/vk/vk_dlaa.c` `hudless()`); the menu is not generated, since generation pauses while it
is open. The device creates two more queues for it (`runtime/vk/vk_device.c` `create_device()`, which asks
`vk_fg_queue_count()` here): of the graphics family where it has them; else the present queue comes from another family
that can present, and the acquire queue is a second queue of that family or of any other, or the present queue itself
when there is none.

The state, `F`:

- `pfam`, `afam`: the queue families of the present and acquire queues.
- `sc`, `fg`: the swapchain context and the frame generation context.
- `fg_max`: the context's largest render size.
- `generated`: generation callbacks: frames AMD's swapchain generated.
- `st`: each frame by the game's state (`runtime/vk/vk_fg_stats.h`), for the periodic line.
- `timed`, `gen_timed`: GPU time measured (`runtime/vk/vk_fg_time.h`), and the generation's with it.
- `on`: the last configured frame had generation on (`vk_fg_generating()`).
- `cuts`: each frame id's camera cut, for the generation callback (`runtime/vk/vk_fg_reset.h`).
- `prepared`: the last frame was prepared (generation on with its inputs).
- `vram`: the frame generation context's video memory as the library reported it, 0 not told.

### `submit_game()`

Takes the queue lock: the game's queue is shared with the recorder and the present path.

### `generate()`

The swapchain's own reset (a frame id that did not follow the last) stays; a cut `vk_fg_frame()` noted is added. The
generation's GPU time is taken here, with the preparation's (`runtime/vk/vk_fg_time.h`).

### `use4()`

`NX2SARIA_FSR4_FG` (default 0, live): 1 takes the SDK 2.4's frame generation with FSR 4. FSR 4 is the upscaler only
while one is set (dlaa): without an upscaler the FSR 3.1 library's runs.

### `vk_fg_wanted()`

Follows the setting: `runtime/gfx/gfx_present.c` `window_settings_live()` remakes the swapchain when this stops matching
`vk_fg_active()`.

- With or without an upscaler (`runtime/vk/vk_dlaa.c` makes the motion vectors either way, `fg_only_frame()`), and with
  the FSR 3.1 library: a build without it keeps the plain swapchain whatever the setting says, with one line in the log.
  Outside Windows there is no AMD library: `runtime/gfx/gfx_present.c`'s presenter runs the runtime's passes
  (`runtime/vk/vk_fsr3fg.c`).
- Not on a device made through Streamline's proxies (NVIDIA's frame generation set at the start, `vk_dlssg_dpa`): AMD's
  swapchain there crashed in NVIDIA's driver when the menu switched from NVIDIA's frame generation to AMD's. That switch
  waits for a restart, as the menu says (`runtime/ui/menu.cpp` `row_framegen()`).
- Not after a failure (`fail()`): the present code then takes AMD's swapchain down as it does for a switch in the menu,
  and a plain one follows.
- Not without a queue to present on besides the game's (`runtime/vk/vk_device.c` `create_device()` says why): the plain
  swapchain stays.
- Two libraries can make the swapchain: the FSR 3.1 library, here, or AMD's FSR SDK 2.4 (`runtime/vk/vk_fg4.cpp`) where
  the setting asks for it while FSR 4 is the upscaler (`use4()`). The swapchain in place is wanted only while its
  library is still the one to use: a change of that setting or of the upscaler between FSR 3.1 and FSR 4, or a failure
  of the SDK 2.4 path, takes it down as switching frame generation off does, and the next present makes it with the
  other library.

### `vk_fg_queue_count()`

Always the three queues: the setting can be switched on during a run, and idle queues cost nothing.

### `vk_fg_active()`

AMD's swapchain is in place: the window's swapchain handle is AMD's object, and every swapchain call has to go through
AMD's functions. That holds after a failure too, until the present code has replaced the swapchain (`vk_fg_wanted()`):
the plain Vulkan calls must never get that handle.

### `vk_fg_create_swapchain()`

- The SDK 2.4 path may fail before its swapchain exists, and says so (`use4()` is 0 from then on): the FSR 3.1 library's
  swapchain is made instead.
- A swapchain context whose swapchain did not come is destroyed, and with it the swapchain it made: nothing was handed
  out yet.

### `vk_fg_hdr_metadata_fn()`

The HDR10 metadata goes to AMD's swapchain through its own call (`runtime/vk/vk_hdr.c` `vk_hdr_metadata()`).

### `hdr_format()`

An HDR10 swapchain (10-bit PQ or 16-bit float): frame generation is told its inputs are HDR.

### `vk_fg_frame()`

- `vk_dlaa_hudless_wanted` is set: the upscaling pass keeps a copy of the picture before the HUD (`runtime/vk/vk_dlaa.c`
  `hudless()`).
- The menu first: its frames were counted as frames without depth and motion before, since the inputs were taken first
  (the menu stops the game's frames). They are not taken while it is open; the first frame after it is a cut. `got` says
  the inputs reached this present.
- The context's largest render size covers the depth and motion vectors it is given. They have the scene's size (with an
  upscaler or without), which DLAA and the widescreen pictures make larger than the window, and a context made for the
  window's size alone wrote past its own resources: a crash in `amd_fidelityfx_vk.dll`, device faults, striped frames.
  The scene is never larger than the picture (`runtime/patches/widescreen.c` `wide_picture()`), so the context is made
  for that once; remaking it while the swapchain still generated from the old one hung the present. A frame with a
  larger depth anyway is not generated.
- A new window size: the context is made for its display size, after a wait for the device; the GPU is idle then, and
  frames whose generation never ran are let go (`fgt_idle()`).
- The HUD-less picture has the swapchain's format (`runtime/gfx/gfx_present.c` `hudless_record()`).
- No infinite-depth flag: the game's far plane is finite, and each preparation gives it.
- The context's memory is asked for the memory report (`vk_fg_vram()`); 0 when the provider does not say.
- Only with the picture without the HUD: generated from the presented picture alone, the HUD would be warped along with
  the scene behind it and trail when the camera turns. A frame without that picture is shown as it is.
- A cut: DLSS started a new history, or without an upscaler the same cases did (`vk_dlaa_fg_inputs()`), or the frame
  before was not prepared (generation off for the menu, no inputs), so the frame between the two would be made from a
  stale depth and motion.
- Each frame by the game's state (`runtime/vk/vk_fg_stats.h`): the menu first, then a frame without a scene (the title,
  a movie, a load), then why a gameplay frame was not generated. The line every 600 gameplay frames gives that period's
  counts, the swapchain's presents over its time (the overlay's frame rate) and the GPU time a generated frame took.
- The picture's rectangle: with black bars beside or below it (a window of another shape) a generation over the whole
  swapchain warped the picture's edges into the bars.
- The motion vector scale is 1: render pixels, current to previous (`runtime/vk/vk_dlaa.c` `mv_pass()`), as FSR takes
  them.
- The frame's camera (`runtime/vk/vk_dlaa.c` `proj_planes()`), as the upscaler gets it (`runtime/vk/vk_ffx.cpp`).

### `vk_fg_release()`

- The HUD-less picture is made for frame generation alone: a frame generator that runs again asks for it again
  (`vk_fg_frame()` sets `vk_dlaa_hudless_wanted`); left set, it was made and copied every frame after frame generation
  went off, and kept.
- AMD's contexts are gone and the device idle: nothing records into the timestamps, which are released
  (`fgt_release()`).
- The next swapchain counts its presents from 0: the period of the statistics starts again, the run's totals stay.

### `vk_fg_vram()`

The frame generation contexts' video memory, FSR 3.1's and FSR 4's (`vk_fg4_vram()`), for the memory report
(`runtime/vk/vk_ffx.cpp` `vk_ffx_vram()`). The swapchain's own images are counted with the swapchain
(`runtime/gfx/gfx_present.c`).

## `runtime/vk/vk_fg4.cpp`

Frame generation through AMD's FSR SDK 2.4: `amd_fidelityfx_loader_vk.dll` (`runtime/vk/vk_ffx4.cpp` loads it) with
`amd_fidelityfx_framegeneration_vk.dll` next to the executable. `runtime/vk/vk_fg.cpp` takes this path instead of the
FSR 3.1 library's where the setting asks for it while FSR 4 is the upscaler (`runtime/vk/vk_fg.cpp` `use4()`,
`NX2SARIA_FSR4_FG`), and its functions here are called by `runtime/vk/vk_fg.cpp` alone. The loader picks the frame
generation and the swapchain for the card: the machine-learned FSR Frame Generation 4.x on Radeon RX 9000 cards with
Windows 11, FSR Frame Generation 3.1.x on every other. The log names both versions.

What happens each frame is what `runtime/vk/vk_fg.cpp` does (the configuration, then the preparation from the frame's
depth and motion vectors, then the generation from the swapchain's callback at present, the order FSR Frame Generation 4
requires), in this SDK's layouts and with what it adds: the camera's position and axes in the preparation, which FSR
Frame Generation 4 needs, and the version each context is made for.

The swapchain's life differs from the FSR 3.1 library's:

- A new size remakes the swapchain in place: its create function takes the swapchain's own handle, and the handle stays.
  `vk_fg4_destroy_swapchain()` therefore does nothing while the swapchain context lives.
- Taking it down (`vk_fg4_release()`) waits for its presents, destroys the frame generation context, then the swapchain
  context, and calls its destroy function last, as the SDK asks.

AMD publishes the SDK's libraries for Windows only: in any other build this path is never available. The file defines
`VK_FFX_NO_API` before it includes `runtime/vk/vk_ffx.h`, as `runtime/vk/vk_ffx4.cpp` does.

The state, `F`, is `runtime/vk/vk_fg.cpp`'s with these:

- `handle`: the swapchain the context made: remade in place, destroyed after the context.
- `sc_desc` to `fg_pin`: what the contexts were made from: the API asks that it stays until the
  context is destroyed.
- `prepared`: the last frame was prepared (a frame after one that was not starts a history).
- `on`: the last configured frame had generation on (`vk_fg4_generating()`).

The probe's state:

- `g_lock`: the menu asks from its thread, the present from its own.
- `g_avail`: -1 not asked yet; 0 this path does not run here; 1 it does.
- `g_sc_list`: the swapchain versions the loader listed, for the log.
- `g_fg_name`: the frame generation version a context gets: the one asked for, else the first listed.
- `g_fg_pin`, `g_sc_pin`: `NX2SARIA_FG4_VERSION`, `NX2SARIA_FG4_SWAPCHAIN` (tests): the version asked for by its id; 0
  the loader's choice.

### `submit_game()`, `generate()`

As in `runtime/vk/vk_fg.cpp`: the queue lock for the game's queue, and the generation's GPU time with the preparation's
(`runtime/vk/vk_fg_time.h`).

### `fail()`

After this the FSR 3.1 library generates the frames: `runtime/vk/vk_fg.cpp` `vk_fg_wanted()` stops wanting this
swapchain, the present code takes it down as it does for a switch in the menu, and the next one is made by the other
path.

### `list_versions()`

The list the loader gives for one kind of context, as text, and the id of the version a test asks for by the start of
its name (0: none asked, or not listed).

### `provider()`

The version a context runs, as its provider names it ("3.1.6"); "?" when it does not say.

### `hdr_format()`

An HDR10 swapchain (10-bit PQ or 16-bit float): frame generation is told its inputs are HDR.

### `wait_presents()`

The swapchain's own presents are done before it is remade or taken down.

### `vk_fg4_available()`

The loader and its frame generation library are beside the executable and list a frame generation and a swapchain for
the device. Asked once, with the device; 0 before it exists. It is asked several times a frame, so the answer is an
atomic read. A build that does not ship `amd_fidelityfx_framegeneration_vk.dll` beside it answers 0.

The menu's name for it (`vk_fg4_name()`) is the first of the list, which the loader takes when none is asked for (a
test's version changes the context, not this name); a version the driver brings carries a star.

### `vk_fg4_active()`

The SDK's swapchain is in place: the window's swapchain handle is its object, and every swapchain call has to go through
its functions, after a failure too, until the present code has taken it down.

### `vk_fg4_create_swapchain()`

- A new size: the swapchain is remade in place and keeps its handle.
- The version this code was built against, which the SDK asks for with every context, and a test's version.
- The swapchain the driver brings (listed with a star, the loader's choice where it is listed) hands out no destroy
  function: its context's destruction is the swapchain's too (`vk_fg4_release()`).
- A swapchain made whose context then failed is destroyed through that function: nothing was handed out yet.

### `vk_fg4_destroy_swapchain()`

Nothing while the swapchain context lives: a new size remakes the swapchain in place (`vk_fg4_create_swapchain()`), and
`vk_fg4_release()` destroys it after its context.

### `vk_fg4_frame()`

- `vk_dlaa_hudless_wanted` is set: the upscaling pass keeps a copy of the picture before the HUD (`runtime/vk/vk_dlaa.c`
  `hudless()`).
- The menu is asked first, and the inputs are not taken while it is open (`runtime/vk/vk_fg.cpp` `vk_fg_frame()`).
- The context's largest render size covers the depth and motion vectors it is given: the picture's size, which the scene
  never exceeds (`runtime/vk/vk_fg.cpp` `vk_fg_frame()` has the history of this rule).
- A new window size: the context is made for its display size, after a wait for the device; frames whose generation
  never ran are let go (`fgt_idle()`).
- The HUD-less picture has the swapchain's format (`runtime/gfx/gfx_present.c` `hudless_record()`).
- Only with the picture without the HUD: generated from the presented picture alone, the HUD would be warped along with
  the scene behind it and trail when the camera turns. A frame without that picture is shown as it is.
- Each frame by the game's state, and the periodic line (`runtime/vk/vk_fg.cpp` `vk_fg_frame()`).
- The picture's rectangle: with black bars beside or below it (a window of another shape) a generation over the whole
  swapchain warped the picture's edges into the bars.
- The motion vector scale is 1: render pixels, current to previous (`runtime/vk/vk_dlaa.c` `mv_pass()`), as FSR takes
  them.
- The game's projection: near 0.1, far at infinity (`runtime/vk/vk_ffx.cpp`).
- `viewSpaceToMetersFactor` is 1: the game's world is taken to be in metres (its near plane is at 0.1). With 0.01 here,
  the value `runtime/vk/vk_fg.cpp` gives the FSR 3.1 library, frame generation 3.1.6 drew grey translucent shapes into
  the sky of its frames while the camera turned, and with 0 it smeared them; with 1 its frames were clean in the same
  camera sweep.
- The camera in the world: the game's own, or one at the origin looking down -z on a frame without it.

### `vk_fg4_release()`

The next swapchain counts its presents from 0 (`runtime/vk/vk_fg.cpp` `vk_fg_release()`).

### `vk_fg4_vram()`

The frame generation context's video memory, for the memory report (`runtime/vk/vk_fg.cpp` `vk_fg_vram()`).

## `runtime/vk/vk_fg_pose.inc`

Included by `runtime/vk/vk_dlaa.c`: the final submitted pose inputs for owned player models. Replay workers change
history under the motion-list lock; the submitting thread selects and consumes it after the group finishes. Model and
shape identities come from producer-side ownership records, never from a mesh fingerprint.

`POSE_BANKS` (5): the uniform banks c3 through c7.

### `pose_owner()`

- Only vertex resources consumed by this shader can establish identity. Stale unused bindings cannot.
- The per-shape record is required even for skinned meshes; one palette can serve several pieces.

### `pose_match()`

The caller holds the motion-list lock while parallel replay workers record draws.

### `pose_previous()`

- The same mesh is drawn into several depth targets. Only the resolve knows which view supplies FG.
- Unused bindings retain the previous draw's values. Only the shader's vertex inputs identify this mesh.
- The same instance can be submitted again in the same pass. A second instance of this mesh can use another palette, or
  rewrite a palette at the same address between submissions.

### `pose_rotation_trace_limit()`

C6 holds a part's submitted transform. Translation is deliberately excluded: a fast jump does not itself make a vector
unreliable. Only a positively identified sharp rotation opts in.

- The threshold is `NX2SARIA_MOTION_GUARD_DEGREES`, `motion_guard_degrees` in the settings file too.

### `pose_matrix_sharp()`

Two 3x4 matrices (rows of four floats) of one part or bone, this frame's and last frame's: 1 when both are plain
rotations and the turn between them is past the guard threshold. `runtime/vk/vk_skin_motion.inc` asks it for each bone
of a palette.

- trace(R1 \* transpose(R0)) = 1 + 2 cos(angle). The selected threshold controls when this part needs visibility
  filtering; equipment retains its separate raw policy.

### `pose_select()`

- Selection preserves each part independently. No six-frame hold and no queue-wide drop: a risky body triangle cannot
  suppress the shield, sword, or another well-behaved part.

### `pose_status()`

The motion status line, every 120 frames with motion vectors: the player's parts (ownership records), then the other
skinned draws (`runtime/vk/vk_skin_motion.inc` `skin_status()`).

## `runtime/vk/vk_fg_reset.h`

Camera cuts for FSR 3.1 frame generation (`runtime/vk/vk_fg.cpp`). DLSS starts a new history on a cut, another scene
size or another target, and `runtime/vk/vk_dlaa.c` `vk_dlaa_fg_inputs()` hands that out with the frame's depth and
motion vectors. FSR 3.1 takes it only in the generation callback (`ffxDispatchDescFrameGeneration.reset`), which its
swapchain calls later, on its own present thread, for the frame id the configuration gave. So `runtime/vk/vk_fg.cpp`
`vk_fg_frame()` notes each frame's cut here under its id, and the callback looks it up. Without it every generated frame
of a cut was blended from two unrelated pictures: an area load or a cutscene's cut flashed a frame of both.

A ring of `FGR_SLOTS` frames; each slot holds the frame id and its cut as one atomic word, so a slot being rewritten
never pairs an old id with a new frame's answer. An id the ring no longer holds (the callback fell that far behind, or
the slot was never written) counts as a cut: a lost history costs one plain frame, a wrong one blends two. Own header so
`tests/fg_reset_check.c` checks it without a GPU.

`FgrRing`: a `slot` is (frame id << 1) | cut, written by `vk_fg_frame()`; 0 never matches (ids start at 1). Frame ids
above `FGR_MAX_ID` do not fit beside the cut bit; they always count as a cut.

## `runtime/vk/vk_fg_stats.h`

What happened to each of the game's frames under frame generation, by the state the game was in, for the log line both
FSR 3.1 paths write (`runtime/vk/vk_fg.cpp` and `runtime/vk/vk_fg4.cpp` through AMD's libraries on Windows,
`runtime/vk/vk_fsr3fg.c` through the runtime's passes elsewhere). One counter a frame, in one of three states:

- Gameplay: the game drew a scene for the frame (`runtime/vk/vk_dlaa.c` armed it). Generated, or why not: a history that
  starts at this frame (a camera cut, a new size, the first frame after a pause), no depth and motion vectors for it, no
  picture without the HUD, or a scene larger than the generation context;
- Menu: the settings menu was open (generation pauses), whether a scene was drawn behind it or not;
- No scene: the game drew no scene (the title, a movie, a load, a fade to black, the file menu), so there was nothing to
  generate from.

The counters before this were one total from the start of the run, and the frames of the title, the opening movie and
the file menu, about 4600 at 60 fps, came first: a log read at 21600 frames said 13719 generated, 63%, while the same
log's gameplay periods generated 600 of 600. The settings menu's frames were counted as frames without depth and motion,
since the inputs were tested before the menu.

A line is due after `FGS_PERIOD` gameplay frames, or after `FGS_IDLE` frames of any kind (a long movie or menu still
reports), and gives that period's counts, the presents the screen got in it and its time (presents over seconds is the
frame rate the performance overlay shows with frame generation, `runtime/gfx/gfx_present.c` `output_rate()`), the GPU
time a generated frame took, and the run's totals. The first `FGS_LINES` lines are all written, then one period in
`FGS_EVERY`, each with the totals, so a long session neither floods the log nor loses a count.

Own header so `tests/fg_stats_check.c` checks it without a GPU.

- `FGS_PERIOD` (600): gameplay frames a line.
- `FGS_IDLE` (3600): frames of any kind a line, when gameplay is rare.
- `FGS_LINES` (60): lines written before only one period in `FGS_EVERY` is.

`FgsOutcome`:

- `FGS_GENERATED`: gameplay: prepared with generation on, from this frame's inputs.
- `FGS_CUT`: gameplay: prepared, but the history starts here (no frame between it and the one before).
- `FGS_NO_INPUTS`: gameplay: the scene was drawn, but no depth and motion vectors reached the present.
- `FGS_NO_HUDLESS`: gameplay: no picture without the HUD.
- `FGS_TOO_LARGE`: gameplay: the scene is larger than the generation context.
- `FGS_MENU`: the settings menu was open.
- `FGS_NO_SCENE`: the game drew no scene.

`FgStats`:

- `n`: this period.
- `total`: the run.
- `presents0`: the screen's presents when the period started (`FgsLine.presents` is the difference).
- `t0_ns`: when the period started.
- `gpu_ms`: GPU time of the generated frames timed in this period, and how many.
- `periods`: periods ended.
- `lines`: lines written.

### `fgs_classify()`

The frame's outcome, in this order: the menu first (whatever the frame had behind it), then whether the game drew a
scene, then why a scene's frame was not generated. scene: the game drew a scene for this frame; have: its depth and
motion vectors reached the present; fits: they fit the context; hudless: the picture without the HUD did; cut: the
history starts at this frame.

- Inputs without the scene flag: the scene was drawn, and the frame is counted as gameplay.

### `fgs_count()`

`presents`: the screen's presents so far (generated frames included); `now_ns`: the time.

- First frame, or a new swapchain whose present count went back to 0: the period's presents and time start here.

### `fgs_gpu()`

The GPU time one generated frame took (its preparation and its generation).

### `fgs_line()`

When a line is due: writes it into out (name: the log's prefix, "fg", "fg4", "fsr3fg") and starts the next period.
Returns 1 when the line is to be logged, 0 when it is due but this period is one of those skipped, -1 when none is due.

## `runtime/vk/vk_fg_time.h`

The GPU time of a generated frame through AMD's libraries (`runtime/vk/vk_fg.cpp`, `runtime/vk/vk_fg4.cpp`), for frame
generation's log line (`runtime/vk/vk_fg_stats.h`): timestamps around the frame's preparation, in the present's command
buffer on the game's queue, and around its generation, in the command list AMD's swapchain hands its generation callback
on its own thread. A frame counts once both are in: `prepare_end` - `prepare_begin` plus `generate_end` -
`generate_begin`.

A ring of `FGT_SLOTS` frames, four timestamps each, reset on the host after they are read (`hostQueryReset`), so nothing
is recorded into AMD's command list but the two timestamps. A slot's state moves free -> prepared -> generated on the
two threads by compare-and-swap; a slot still busy when its frame comes round again leaves that frame untimed, and a
prepared frame AMD never generated (generation off before its present) is let go `FGT_SLOTS` / 2 frames later.
`NX2SARIA_FG_GPU_TIME=0` turns it off; without `hostQueryReset` or timestamps on the game's queue it stays off, and the
generation is timed only where the present queue's family has timestamps too.

- `fgt_init()`: after the device exists: `present_family` is the family of the queue AMD's swapchain presents on. 1 when
  frames are timed, `*gen` whether their generation is (else the preparation alone). Logs one line the first time.
- `fgt_prepare_begin()`, `fgt_prepare_end()`: timestamps around the preparation of frame `id` in cb; begin returns 0
  when this frame is not timed (end is then skipped).
- `fgt_generate_begin()`, `fgt_generate_end()`: the same around its generation, in the callback's command list (any
  thread); a frame not prepared with timing is skipped.
- `fgt_poll()`: frames whose times are in: up to `max` of them into `ms[]`, their slots free again. `now_id`: the newest
  frame id.
- `fgt_idle()`: the GPU idle (a context made again): slots whose timestamps will never all come (a generation AMD did
  not run) are let go; one being recorded on another thread is left for its owner.
- `fgt_release()`: the device idle.

## `runtime/vk/vk_fg_time.c`

The GPU time of frame generation through AMD's libraries: `runtime/vk/vk_fg_time.h` says what is timed and how.

### `fgt_init()`

- The state is zeroed at the first use, or after `fgt_release()`.
- The generation runs on a queue AMD's swapchain picks: the game's or the present queue, timed where both have
  timestamps.

### `fgt_prepare_begin()`

A slot not free is still in use by a frame `FGT_SLOTS` ago: this frame is not timed.

### `fgt_poll()`

- Generated only where the generation is timed; a prepared frame not generated after half the ring is let go once its
  preparation is done (nothing writes its slot any more).
- A generation recorded but not done yet waits: its queries may still be written, so they are never reset early.

### `fgt_release()`

After AMD's contexts are destroyed (`runtime/vk/vk_fg.cpp` `vk_fg_release()`), with the device idle: nothing records
into the pool.
