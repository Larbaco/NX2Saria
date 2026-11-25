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
