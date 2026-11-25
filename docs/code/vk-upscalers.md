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
destroyed once the GPU is past the submission of its last upscale (`g_dead`, a list of `runtime/vk/vk_up_defer.h`), as
`runtime/vk/vk_ffx.cpp` does with the library's contexts. The constants live in a host-visible ring of `RING` (256)
slots, each one `Consts`, `SpdConsts` and the RCAS words; a slot is reused `RING` calls later, after the GPU passed the
submission that last read it.

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

## `runtime/vk/vk_aa.c`

The anti-aliasing setting (`NX2SARIA_AA`, `aa` in `nx2navi.ini`): what runs in place of the game's own SMAA.

The game anti-aliases every frame with SMAA in three compute passes right after the bloom composite: edge detection into
4x4-pixel tiles, blending weights from the tiles (an indirect dispatch, with the game's area lookup texture), and the
composition, which reads the composite and the weights and writes the result into the scene target that the game's
upscaler, the HUD and the output passes then take. The composition stores each channel's square root packed as RGB9E5
words through an integer view of that R11G11B10F target (`runtime/vk/vk_exec_draw.c` `raw_note()`).

`aa = game` (the default) leaves those passes alone. `off`, `fxaa` and `smaa` leave out the edge and weight passes and
replace the composition (`runtime/vk/vk_exec_draw.c` `aa_dispatch()`) with one of the passes here, which read the same
composite and write the same words: off stores it as it is, fxaa smooths it first (`runtime/shaders/aa_post.comp`), smaa
runs SMAA at its Ultra preset (`runtime/shaders/smaa_*.comp`, the reference implementation in
`runtime/shaders/smaa.glsl` with the area and search textures of `runtime/third_party/smaa/`). `taa` is DLSS or FSR at
the scene's own size: the menu sets the upscaler for it (`runtime/ui/menu.cpp` `row_aa()`), and the game's SMAA runs
with it. While DLSS or FSR anti-aliases or scales the scene (dlaa = 1), game, off, fxaa and smaa still choose the pass
above: in their native modes the upscalers run before the bloom (`runtime/vk/vk_dlaa.c`) and the pass smooths their
picture after it, in the scaling modes the pass runs on the scene first and the upscaler scales its result. off then
leaves the upscaler's TAA alone on the edges.

The game's passes are known by a hash of their guest code (`vk_aa_role()`), which does not change with the translator or
its settings. Everything here runs on the replay thread.

`GAME_SMAA`: FNV-1a over the guest code of the game's three SMAA compute programs (`runtime/vk/vk_exec_draw.c`
`cs_guest_hash()`), game 1.1.1, found with `NX2SARIA_AA_LOG=1`: the edges (a 4x4-pixel tile per invocation), the weights
(an indirect dispatch over the tiles with edges) and the composition (16x16 pixels per group).

### `temporal()`

DLSS or FSR anti-aliases this frame's scene: dlaa = 1 and one of them runs (`runtime/vk/vk_dlaa.c`).

The setting's state, `A`:

- `frame_read`: atomic: the frame whose first `vk_aa_mode()` call took the setting.
- `mode`: atomic: this frame's (`vk_aa_mode()`).
- `said`: taa was last logged as waiting for DLSS or FSR.
- `composed`: the last composition seen was replaced (`vk_aa_composed()`): the passes before it may go.
- `frames_on`, `composes`: frames with a replacement asked for, compositions replaced.

### `vk_aa_mode()`

Every compute dispatch asks (`runtime/vk/vk_exec_draw.c` `aa_dispatch()`), on whichever thread records it, and the
replay workers record at once: the first call of a frame claims it with a compare-and-swap, and only that one frees the
idle work images (`work_idle()`, which two threads would have freed twice) and takes the setting. A call that finds the
frame claimed gets the mode as it stands.

- `taa` is the game's SMAA with DLSS's or FSR's TAA; without them at work, the game's SMAA alone.
- A game version or a frame without the passes this file knows: nothing is replaced, and the log says so once.

### `vk_aa_composed()`

A composition that could not be replaced runs as the game's, and the edge and weight passes run again from the next
frame on, until a composition is replaced again: the game's composition must never read weights left out.

### Pipelines

`P`:

- `tables`: the area and search textures: 0 not made, 1 ready, -1 failed.
- `luma`, `edges`, `weights`: at the scene's size (w, h).
- `smaa_frame`: the last frame that recorded SMAA's passes.

### `work_idle()`

SMAA's work images stayed for the rest of the run once aa left smaa: three pictures of the scene's size, 109 MiB at
5120x2142. They go `WORK_IDLE` presents after the last frame that used them, through the deferred destruction (no
submission still names them then); smaa again makes them as a change of the scene's size does (`work_images()`), in the
command buffer of its first frame. A minute, not less: the passes run only where the game runs its own SMAA, so a long
pause or loading screen with smaa set also counts as unused, and the images are made again after it.

### `img_make()`

An image of the passes' own, in the GENERAL layout from cb on (the transition is recorded into cb).

### `tables_make()`

The area and search textures, uploaded once through the upload batch and waited for (the first SMAA frame only).

### `work_images()`

SMAA's work images at the scene's size, made again when it changes.

### `pass()`

One pass: its pipeline, its images (samplers: linear but for the search texture) and the push constants.

The descriptors are pushed through the recorder (`runtime/vk/vk_rec.h`).

## `runtime/vk/vk_contact.c`

Screen-space contact shadows (`NX2SARIA_CONTACT_SHADOWS`, percent strength, 0 = off). The game fills a full-resolution
G-buffer whose targets include a B10G11R11 scene colour, an R32F linear depth (view distance / far) and a D32F depth
(`is_gbuffer()`), lights it in half resolution, and then composites the lighting into that scene colour in one pass
whose only target is the scene colour, with a D16 depth that sorts pixels into material classes (`is_composite()`). When
the pass after the composite begins, or a dispatch or copy comes first, `runtime/shaders/contact_shadow.comp` marches
each pixel a short way toward the sun through the linear depth and darkens the scene colour where the ray passes behind
nearby geometry: the small shadows at feet, under stones and in grass that the shadow maps are too coarse for.
Transparent surfaces, the sky and post-processing come after, untouched.

Inputs from the game, read from the fragment stage's uniform block c4 bound in the composite pass (guest memory, so it
may be one frame newer than the replay): the projection scales `c4[7].x` and `c4[8].y`, the far plane `c4[14].y`, and
the first sun cascade's depth row `c4[44].xyz`, whose negated direction points from a view-space position toward the sun
(the G-buffer shaders compare `dot(c4[44].xyz, pos) + c4[44].w` with the cascade map). The scene colour images get
STORAGE usage when the setting is on (`runtime/vk/vk_texture.c` `create_tex()`).

`C`:

- `scene`, `depth`: the last G-buffer's scene colour and linear depth.
- `pending`: the composite into scene ran; the march waits for the next pass.

### `init()`

`NX2SARIA_CONTACT_DEBUG=1`: the frame shows the contact shadow factor in grey.

### `is_gbuffer()`

A G-buffer pass: a B10G11R11 scene colour, an R32F linear depth and a D32F depth, all one size, at least 640 wide.

### `vk_contact_flush()`

- The submitting thread past a group (`runtime/vk/vk_exec.c` `NX2SARIA_REPLAY_PIPE`): the group's first chunk may have
  made the pass due and a later one records it where the replay would; only one still due after the group is this one's.
- A barrier before the march: the compose passes' writes come before it reads and writes the scene colour. One after it:
  what follows sees the darkened colour.
- The game's bound compute pipeline and descriptors were replaced: `vk_exec_draw_new_cb()` has them bound again.

### `is_composite()`

The lighting composite: the remembered scene colour as the only target, with a D16 depth of its size.

### `vk_contact_pass_begin()`

- The camera block: c4 of the fragment stage, 0x940 bytes. The composite binds it only after its pass begins, so the
  newest one seen at any pass start serves.

## `runtime/vk/vk_tonemap.c`

Decode-and-scale pass standing in for the scene upscaler (`runtime/vk/vk.h` `vk_tonemap_record()`; used by
`runtime/vk/vk_exec_draw.c` `untranslated_dispatch_fallback()`).

The game's upscaler (tensor-core, untranslatable) reads the post pass's picture, whose channels hold the square root of
their linear value packed as RGB9E5 (prog 102), and writes the linear picture the HUD is drawn over; the frame's last
pass encodes to sRGB. This pass (`runtime/shaders/fallback_tonemap.spvasm`) scales with linear filtering in the stored
values and squares them into an RGBA16F image of the target's size, which the caller blits in 1:1. The game composites
and tonemaps before the post pass, so nothing here changes exposure. (It was a tonemap once, when the input was taken
for the raw HDR scene; the blit's sRGB decode that stood in since darkened mid-tones and lifted the darkest values.)

The filter: the console's upscaler is a network that rebuilds edges, and a linear filter over a scene drawn at about
three quarters of the picture is visibly softer than that. With `NX2SARIA_STANDIN_FILTER` = easu (the default) the pass
that enlarges is `runtime/shaders/fsr_easu.comp` instead, the edge-adaptive filter the present scales to the window with
(`runtime/vk/vk_fsr.c`), built here with its SQUARE constant set: it filters the same stored values, which are the
perceptual encoding that filter is made for, and squares the result as the linear pass does, so only how the scaled
sample is taken differs. `bilinear` keeps the linear pass. The linear pass also stays where nothing is enlarged (the
filter only enlarges) and where the caller says another upscaler's picture replaces this one
(`runtime/vk/vk_exec_draw.c` `untranslated_dispatch_fallback()`: DLSS or FSR 3.1, `runtime/vk/vk_dlaa.c`). A scene as
large as the picture (`scene_share` = 1, the menu's Native: `runtime/patches/widescreen.c` `plain_share()`) is not
scaled at all: the linear pass samples it with a nearest-texel sampler, so each output texel is its source texel
squared, whatever the filtering hardware's sub-texel precision. The movie's frames do not come through here: the caller
blits those (RGBA8) itself.

The state, `TM`:

- `pipe`: scale with linear filtering, then square (`runtime/shaders/fallback_tonemap.spvasm`).
- `easu`: scale with the edge-adaptive filter, then square (`runtime/shaders/fsr_easu.comp`).
- `filter_logs`: lines `tm_filter_log()` has written.
- `filter_last`: what its last line named: 1 the linear filter, 2 the edge-adaptive one, 3 a copy 1:1.
- `nearest`: the same with nearest-texel filtering: a source of the output's size (copy 1:1).
- `out`: the pass's own output (RGBA16F, the target's size); the caller blits it in.

### `tm_out()`

The output image at w x h, made again when the size changes (the old one is left to the deferred release).

### `tm_init()`

The push constant range is 32 bytes: the linear pass reads 24, `tm_easu()`'s 32.

### `tm_easu()`

The edge-adaptive pipeline, made at the first frame that asks for it (a run that never does, with the setting at
bilinear or DLSS on, never builds it): the present's EASU shader with specialization constant 0 (SQUARE) set, on this
file's layout, whose two bindings are that shader's. `VK_NULL_HANDLE` when it cannot be made; the linear pass then
stays.

### `tm_filter_log()`

One log line when the filter the pass scales with changes (the setting, DLSS or FSR switched on or off, a scene as large
as the picture), 8 at most: a scene whose size changes from frame to frame can cross the picture's size often.

### `vk_tonemap_forget()`

Views of the images the pass samples are kept as long as the image: `vk_tonemap_forget()` drops those of an image
`fallback_scratch` frees, and those of a game image the pass read as it is when the game's texture goes
(`vk_exec_image_gone()`), which also gives their places back. Without the second, a game image's view stayed naming a
destroyed image whose handle a new image could get. The texture's destroy runs on any thread, hence `tm_view_lock`.

### `vk_tonemap_record()`

Records the pass from src (sampled, sw x sh) into the pass's own RGBA16F image of dw x dh and returns it (the caller
blits it into the target); `VK_NULL_HANDLE` when it cannot (the caller blits the source instead). edge: the picture is
what the screen shows, so the edge-adaptive filter may scale it (0: another upscaler's picture replaces it).

- `NX2SARIA_TM_DISPATCH=0`: everything but the dispatch; `=2`: not at all (diagnosis).
- `NX2SARIA_STANDIN_FILTER`, read each frame (the setting is live). The edge-adaptive filter only enlarges: a target
  smaller than the source in either direction, or of its size, keeps the linear pass.
- The same size both ways: each output texel takes its own source texel, through the nearest-texel pipeline.
- The descriptors are pushed through the recorder (`runtime/vk/vk_rec.h`), in order.
- `runtime/shaders/fsr_easu.comp`'s constants: the part of the source that is the picture and the source's size (the
  whole image both times, as the linear pass maps it), then the output's size and the picture's corner in the source.
  Each of its groups writes a 16x16 tile.

## `runtime/vk/vk_mvcheck.c`

`NX2SARIA_MV_CHECK=<frames>` (test): measures how well DLSS's motion vectors explain the picture, without screenshots or
judging by eye. Every `<frames>` DLAA frames, `runtime/vk/vk_dlaa.c` `mv_pass()` copies to host memory the scene light
target of that frame and of the frame before (before DLSS, so jittered), and the motion image twice: after the camera
pass (camera only) and after the per-object pass (what DLSS gets). Once the GPU is done, the completion thread compares
them:

- Warp error: the frame before, sampled bilinearly at p + mv + d, against this frame at p, as a relative luminance
  difference |a - b| / (a + b) averaged over pixels; d = last frame's jitter minus this frame's (the vectors are between
  unjittered positions, the pictures are jittered: content at x is drawn at x + jitter). Lower is better, and the
  correct vectors give the lowest. Over every pixel: zero motion, the camera's and the final vectors, and the final ones
  with d subtracted or left out (the jitter convention). Over the object pixels (final and camera differ by more than a
  quarter pixel, `OBJ_MIN`): the camera's, the final ones and variants of the object part (final - camera) scaled by k =
  -1, 0.5, 1.5, 2 or with one axis negated. The variant with the lowest error names the correction the object vectors
  need, if any ("best").
- Flow: on up to `FLOW_MAX` object pixels with some texture, the offset whose 5x5 luminance patch of the frame before
  matches best, searched over a window around both the camera and the final vector and refined to a sub-pixel by a
  parabola: the measured motion. The end-point error of the camera and final vectors (+ d) against it, median in render
  pixels, and how often the final vector is the nearer.
- Output (DLSS's picture, which the vectors are for): on about `OUT_SAMPLES` pixels picked independently of the vectors,
  the picture's own motion is measured with a wide search (12 px around the camera vector); where it differs from the
  camera's by over 1.5 px the pixel moves on its own. At each sample, DLSS's output (at the same unjittered place)
  against this frame's input, as a relative luminance difference: on the moving samples it grows where DLSS drags old
  colour along wrong vectors (ghosting, smearing), on the static ones it is the plain difference anti-aliasing and
  upscaling make. Their ratio compares runs with the per-object pass on and off (it does not depend on the pass's own
  vectors) and modes.

One `mv_check:` line per check. Disocclusion and changing light add error to every candidate alike, so the comparison
between candidates counts, not the absolute values. Copies and analysis cost a few ms on check frames only.

The state, `K`:

- `period`: 0 off, -1 not read yet.
- `ow`, `oh`: the size of DLSS's output in `OUT`, 0 none this check.
- `want_out`: the check frame's output is to be copied (`vk_mvcheck_output()`).
- `masks`: the check frame's motion draws (`vk_mvobj_frame_masks()`).
- `n`: camera-stage calls (DLAA frames with motion).
- `prev_frame`: the frame the `PREV` colour is of, 0 none.
- `check`: this frame is a check frame (camera stage copied `MV_CAM`).
- `busy`: analysis pending: the buffers are not touched.

### `ensure()`

Host buffers of at least the size asked for; a larger size gets new ones and leaves the old (test only).

### `ufloat()`

Unsigned 5-bit exponent mini-float.

### `sample()`

Bilinear, pixel index coordinates.

### `parabola()`

Offset of the minimum of three samples at -1, 0, 1.

- The offset is kept within half a sample: a flat curve says nothing past the neighbours.

### `flow()`

The object pixels' measured motion (sub-pixel), searched over the box around both vectors with a margin of 3; 0 when the
patch is flat or the match is not clearly better than the box's average.

### Totals over every check

Logged every `TOTAL_EVERY` (20) checks: end-point error histograms in `HBIN` (0.05) px bins of the object samples
(camera, final) and of the static samples (the measurement's own noise floor), and the object warp errors. `KN` (120)
bins hold the histogram of per-sample scale ratios, -2 to 4 in steps of 0.05; `T.cal` counts the checks where k = 0.5, k
= 1 and k = 1.5 fit the static pixels best.

### `hist_add()`

NaN and huge errors go in the last bin.

### `hist_q()`

Quantile q of histogram k, bin centre.

### `mix()`

A fixed pseudo-random pick of pixels, spread over the picture.

### `analyse()`

- The object pixels first, for the sampling rates: about `FLOW_MAX` object and `FLOW_MAX` / 4 static samples.
- `huge` counts camera vectors over 256 px or not finite.
- Object samples the final vector misses by more than 1.5 px where the camera's fits better: where and how.
- Calibration: static pixels with a small camera vector (known right), at k = 0.5, 1, 1.5 of it.
- Where the object part is over half a pixel: the scale of the object part the measured motion shows.
- DLSS's output against this frame's input, on samples moving on their own and static ones (the picture's motion).
- A check counts as wrong when the object pixels are over 0.5 % of the picture and the final vectors are not the best
  fit.

### `vk_mvcheck_output()`

After DLSS ran on a check frame (`runtime/vk/vk_dlaa.c` `evaluate()`): its output (RGBA16F, ow x oh) for the analysis,
which runs once the submission is done.

## `runtime/vk/vk_mvprev.c`

Last frame's object and bone blocks, for per-object motion vectors (`NX2SARIA_DLAA_OBJMV`, on with DLAA). The banks kept
start as c5, c7 and c14 and grow with what the vertex shader variants read (`vk_mvprev_want()`); c7 and c5 are described
here, the others follow the same two-copy rule.

The scene's vertex shaders read an object's place from bank c7 (`gsys_shape`: one 3x4 matrix at the start of a 0x100
block) and a skinned object's bones from c5 (`gsys_skeleton`: a run of 3x4 matrices), of this frame only. The game
writes both into two copies that take turns, the second right after the first: an object's block is at addr in one frame
and at addr +- step in the next, step its size rounded up to 256 bytes (`runtime/vk/vk_mvsurvey.c` measured it: every c7
pair 0x100 apart, c5 pairs their rounded size apart, the pair's contents the same object moved a little). The copy this
frame does not write still holds last frame's when the frame is submitted, so the uniform snapshot
(`runtime/vk/vk_usnap.c`) also copies both neighbours of these blocks, and `vk_mvprev_find()` picks last frame's:

- self: the block was bound at the same address last frame too (an object the game did not write again): itself;
- side: one neighbour was bound in that bank last frame: that one;
- nearer: both were (the copies of adjacent objects interleave): the one with more of its first 16 words equal to this
  frame's;
- none: neither: a new object, no motion of its own.

The pick is checked against what was snapshotted of that block last frame (intact: the same first `CHECK` bytes), and a
summary goes to the log every `SUMMARY` frames:

```
mv_prev: c<n>: <blocks> blocks: self a %, side b %, nearer c %, none d %; intact e %; moved: still f %, under one unit g %, more h %
```

`BONES` (2), `PLACE` (4) and `CLOCK` (11) are NVN binding indices: banks c5, c7 and c14; `BANKS` (14) covers c3..c16.

`Seen` is 16 bytes, so a table of `TABLE` entries stays in the cache: every scene draw probes it (the bytes of
`NX2SARIA_MVPREV_CHECK` live in a table of their own, by entry index, allocated only for the check). An `addr` of 0 is
an empty entry.

The state, `P`:

- `on`: 1 with the knob on and DLAA on this frame.
- `knob`: -1 not read.
- `t`: this frame's bound blocks, last frame's.
- `bytes`: `NX2SARIA_MVPREV_CHECK`: the first `CHECK` bytes of each entry of `t[k]`, NULL without the check.
- `timed`: `NX2SARIA_MVOBJ_TIMES`: the summary's times.
- `st`: per binding index.
- `last`: the block the last scene draw bound per binding index.
- `mask`: binding indices kept (`vk_mvprev_want()`): bones, place and clock from the start.
- `ns_draw`, `ns_frame`: time in `vk_mvprev_draw()` and `vk_mvprev_frame()` since the last summary.

### `vk_mvprev_want()`

`NX2SARIA_MVPREV_GROW=0`: c5, c7 and c14 only (test). The mask grows with an atomic or: the chunks of a parallel group
ask at once.

### `moved()`

Translation distance between the first matrices of two blocks (rows' fourth floats).

### `vk_mvprev_find()`

With both neighbours bound last frame, the copies of adjacent objects interleave: the one with more words equal to this
frame's is taken.

### `vk_mvprev_draw()`

Without `NX2SARIA_MVOBJ_TIMES` the draw is not timed: the clock reads were 1 % of the render thread.

### `mvprev_draw_()`

`draw_lock`: the replay workers' scene draws note blocks at once, but only while a parallel group
(`runtime/vk/vk_exec.c`) records do several threads note at once, so the lock is taken only then (`vk_exec_shared()`).

### `mvprev_draw_locked()`

- `NX2SARIA_MVPREV_CHECK=1`: the summary's checks (3.6 ms a frame); without it only the address is noted.
- The draws that follow keep the block the last one bound: it is noted already.

### `vk_mvprev_frame()`

- Without DLSS (no NGX, no RTX GPU, dlaa = 0) and without frame generation alone (`vk_dlaa_fg_only()`) nothing reads
  last frame's blocks: `P.on` stays 0, no tables.

## `runtime/vk/vk_mvsurvey.c`

Motion-vector survey (`NX2SARIA_MV_SURVEY=1`, off by default): measures whether per-object motion vectors can be made
from the uniform blocks the scene's draws bind. Per-object vectors need each object's matrices of the previous frame;
the game keeps none (its shaders read `gsys_shape`, `gsys_skeleton` and `gsys_projection` of this frame only), so they
would have to be kept from the previous frame's uniform snapshot (`runtime/vk/vk_usnap.c`). That works when a block's
guest address stands for the same object from one frame to the next, which this survey measures.

For every scene draw (vertex stage, scene depth bound: D32 at least `DLAA_SCENE_MIN_W` wide, as `runtime/vk/vk_dlaa.c`
`vk_dlaa_draw()`), each user bank (c3 + index) with a binding is noted: address, size and its first `SNAP` bytes as of
the submit (the snapshot's copy). At each frame the bank's blocks are compared with the previous frame's and the one
before:

- repeat1 / repeat2: blocks whose address was bound in that bank one / two frames ago;
- same / near / far: of the blocks repeated from last frame: bytes unchanged / every float within 5 % or 1.0 of last
  frame's (the same object, moved a little) / some float further (another object, likely).

The game writes its per-object blocks into two copies that take turns, so a moving object's block is at one address in
even frames and another in odd ones. To find the rule that pairs them, a bank's blocks bound two frames ago but not last
frame (this frame's copy) are sorted by address, and so are last frame's blocks not bound this frame (the other copy);
with equal counts they are paired in order:

- pairs, delta: the paired blocks and the commonest address differences (this frame's minus last frame's);
- psame / pnear / pfar: the pairs' contents, as same / near / far above (near: the same object, moved);
- unequal: frames whose two lists had different lengths (no pairing that frame).

What the bank holds: each block's leading 3x4 matrices (rows of a rotation times a scale: three rows of the same length,
at right angles, finite), counted over the whole block as of the submit:

- mats 0 / 1 / 2-7 / 8+: blocks by that count (one: an object's place, `gsys_shape`; many: bones, `gsys_skeleton`).

Every `SURVEY_FRAMES` frames the log gets two lines per bank with the window's totals:

```
mv_survey: c<n>: <draws> draws, <blocks> blocks (<per frame>), sizes <top three>; repeat1 a %, repeat2 b %; same c %, near d %, far e %
mv_survey: c<n>: pairs <n> (<unequal> frames unequal), delta <top three>; psame a %, pnear b %, pfar c %; mats 0 x<n> 1 x<n> 2-7 x<n> 8+ x<n>, at most <m>
```

`TABLE` (1 << 15): blocks per frame, all banks; a town frame binds about 10,000 uniform ranges.

`Block`: an `addr` of 0 is an empty slot; `have` is the bytes of `bytes` that were in the snapshot; `mats` the leading
3x4 matrices of the whole block, 0 when it was not in the snapshot. `S.t` holds this frame, last frame and the one
before; `S.x` and `S.y` are the pairing's lists: this frame's copy, last frame's.

### `find()`

A full table gives NULL: the block goes uncounted.

### `note_size()`

A fifth size is counted with the last.

### `lead_mats()`

Leading 3x4 matrices: three rows of about the same length (a uniform scale), at right angles, all finite.

### `compare()`

Same object moved a little: every float within 5 % or 1.0 of last frame's.

## `runtime/vk/skin_pair.h`

Which draw of last frame a skinned draw of this frame continues (`runtime/vk/vk_skin_motion.inc`), for the motion
vectors of skinned characters. The player's meshes are known by ownership records (`runtime/input/motion_owner.c`);
every other character's draws have no such record, so a draw is paired with last frame's by what it binds:

- key: the mesh and the program: the vertex buffers its shader reads, the index buffer, the index count, width and base
  vertex, and the vertex and fragment shader (the caller hashes them with `skin_mix()`);
- number: the draw's place among this frame's draws with the same key, counted in the order of the command stream
  (`order`), not the order the replay's threads reached them in: two characters of one model share the key.

A draw is paired with last frame's draw of the same key and number, and with none when anything is in doubt, as a vector
from another character's pose is worse than the camera's:

- `SKIN_NEW`: there was no last frame's list, or the key was not in it (the character appeared);
- `SKIN_COUNT`: the key was drawn a different number of times last frame (one appeared or left: the numbers shifted);
- `SKIN_SIZE`: the palette had another size;
- `SKIN_ADDRESS`: the palette is at neither of the two addresses this key and number last had. The game keeps two copies
  of an object's block that take turns, so one object's palette is at last frame's address (not written again) or at the
  other copy's, where it was the frame before that. A third address is another object in this place: two characters of
  one model changed places in the draw order, or one left as another came. A new character therefore gets its first
  vector in its third frame. The two addresses are remembered across frames without a list, so the frame after such a
  gap pairs again.

Own header so `tests/skin_pair_check.c` checks it without a GPU.

`SkinEntry`:

- `key`: the mesh and program.
- `order`: place in the frame's command stream; unique within a frame.
- `palette`: address of the palette block.
- `bytes`: its size.
- `tag`: the caller's (its index of the draw); the entries are sorted.
- `nth`, `of` (set by `skin_number()`): number among the frame's draws with this key, and how many there are.
- `before`, `prev`, `why` are set by `skin_pair()`. `before`: the last palette address of this key and number other than
  palette; 0 not known.
- `prev`: index of the paired entry of last frame; -1 none.
- `why`: `SKIN_PAIRED` or why not.

### `skin_number()`

Sorts a frame's entries by key and stream order and numbers the draws of each key.

### `skin_find()`

The first entry with key in a numbered list, -1 when there is none; its `of` entries follow each other.

### `skin_pair()`

Pairs this frame's numbered entries with those of the last list made (numbered and paired in its own frame).
`consecutive`: that list is of the frame right before this one; of an older one nothing is paired, and only the
addresses are carried on.

### `skin_near()`

Whether a palette is last frame's a little moved: every bone's translation within limit units of its last one. The
matrices are rows of four floats with the translation last in each row (floats 3, 7 and 11); a 4x4 may hold it in its
fourth row instead (12 to 14), so both places are compared, and the one that is not the translation holds constants. For
keys drawn more than once a frame: two characters of one model whose draw order alternates frame by frame pass the
address rule of `skin_pair()`, and stand apart.

The comparison is written so that a NaN is not near.

## `runtime/vk/vk_skin_motion.inc`

Included by `runtime/vk/vk_dlaa.c`: motion vectors for every skinned character (`NX2SARIA_SKIN_MOTION`, `skin_motion` in
`nx2navi.ini`; game 1.1.1). The player's meshes get their own motion through ownership records
(`runtime/input/motion_owner.c`, `runtime/vk/vk_fg_pose.inc`): a draw is the player's when a block it binds belongs to a
model of the player's actor. People, animals and enemies have no such record, so the camera's motion was all they got,
and DLSS and FSR 3.1 smeared them as they moved. This file gives them the same second draw, with last frame's pose and
camera into the motion vector target, without knowing whose they are. A draw the player's path takes
(`runtime/vk/vk_fg_pose.inc` `pose_owner()`) never comes here.

A skinned scene draw (`skin_pick()`): a draw that passes the scene filter of `mvobj_pick()` (solid, one instance, into a
scene-size depth target with the scene context at c3), whose vertex shader reads a matrix palette
(`runtime/gfx/gfx_shader.c` `reflect_palette()` states that rule and what it matched) from one of the uniform banks c3
to c7, and whose vertex layout feeds each joint input of that shader an unsigned integer attribute. Weights are not
asked for: a mesh that binds each vertex to one joint carries none and moves with its skeleton all the same.

Its second draw uses the vertex shader's last-frame variant (translator patch 0015), which computes the position again
from the blocks bound at bank + 15. The variant's position may read c3 to c7 only, among them the scene context c3 and
the palette, and no storage buffer or texture: as for the player. Each block such a draw binds is kept once a frame, by
address (many pieces share a palette, all share the scene context): its bytes go into the frame's arena for the next
frame, and a stretch of the submission's scratch memory is set aside where last frame's bytes of the same object are put
once that is known (`SkinBlock`).

Which draw of last frame a draw continues is decided when the motion vectors are made (`skin_resolve()`, called from
`vk_fg_pose_prepare()`), not at the draw: the replay's threads reach the draws of one pass in any order, and the number
that tells two characters of one model apart counts in the order of the command stream (`runtime/vk/skin_pair.h`, which
lists the cases that leave a draw to the camera). There the frame's draws are first cut down to the scene's own view
(the camera block and size the motion vectors are made for) and to one draw for each mesh and palette: the game draws a
mesh into a depth pre-pass and again, against a copy of that depth, into the light target, and the draw into the depth
image the vectors are tested against is kept, as for the player; the pre-pass draw stands in where the other could not
be listed (its vertex shader reads storage buffers), and from the next frame on a mesh with a light-target draw has its
pre-pass draw left out at the draw already. Then:

- A key drawn more than once must also have each palette near last frame's (`runtime/vk/skin_pair.h` `skin_near()`,
  `SKIN_NEAR_UNITS`);
- Every bank of the paired draws must have the same size;
- The draws that share a block must agree on which block of last frame it continues, or all of them are left out.

What passes has its blocks' scratch filled with last frame's bytes and stays in the list; the rest is taken out of it
(`MoDraw.skin`), so those pixels keep the camera's motion.

The guard for parts that turn sharply (`motion_guard_degrees`) is chosen at the draw, as it picks the fragment shader: a
draw is checked against last frame's depth (`runtime/shaders/link_motion.frag`) when a bone of its palette turned past
the threshold against last frame's palette of any draw with its key. For a key drawn once that is the player's rule; for
more it errs towards the check.

Cost on the replay thread, for a skinned draw: the palette lookup in the shader's reflection, a hash lookup of the
listed meshes, for each block its position reads a lookup in the uniform snapshot and one in the frame's blocks, a
binary search in last frame's list, and for the first draw that binds a block a copy of its bytes; then what any motion
draw costs (`mvobj_draw_()`). Nothing runs while no upscaler is active (`mvobj_draw()`, `skin_pick()`).

- `SKIN_BANKS` (5): c3 to c7, as `POSE_BANKS`.
- `SKIN_SLOTS` (4096): hash slots of a frame's blocks, listed draws and lit meshes.
- `SKIN_FILL` (2048): entries each holds at most.
- `SKIN_NEAR_UNITS` (1.0f): a palette of the same key more than this from last frame's is another character's.

Why a skinned draw kept the camera's motion: `runtime/vk/skin_pair.h`'s, then this file's.

- `SKIN_NOT_READY`: its variant, fragment shader or pipeline is still being made.
- `SKIN_BANKS_OTHER`: the position reads a bank outside c3 to c7, or not the palette, or the palette is not in a uniform
  bank.
- `SKIN_STORAGE`: the variant reads a storage buffer, a texture or an image.
- `SKIN_SNAPSHOT`: a block is not in the uniform snapshot.
- `SKIN_TWIN`: another draw of the same mesh and palette carries the vector.
- `SKIN_VIEW`: drawn with another camera block or size than the scene's.
- `SKIN_SHARED`: the draws sharing one of its blocks disagree on last frame's.
- `SKIN_FAR`: `skin_near()` said no.
- `SKIN_FULL`: a table is full.
- `SKIN_UNUSED`: the frame's list was not recorded (another submission made the vectors).
`SkinPick`, the answer of `skin_pick()`: the bytes of a bone matrix (0: not a skinned draw) and the palette's bank,
counted from c3.

`SkinBlock`:

- `at`: this frame's bytes in the frame's arena.
- `was`: where last frame's bytes of the same object go; the motion draws bind it.
- `last`: last frame's block it continues: -1 none named, -2 two draws named different ones.
- `sharp_with`, `sharp`: `skin_sharp()`'s answer for last frame's block `sharp_with`.

`SkinDraw`:

- `block`: the frame's block per bank, -1 for a bank the position does not read.
- `palette`, `stride`: the palette's bank, the bytes of a matrix.
- `zw`, `zh`: the depth target it was drawn into.
- `cam`: its scene context block.
- `mesh`, `twin`: hashes: the mesh; the mesh and its palette's address.
- `draw`: its `MoDraw`.

`SkinFrame`:

- `scene_depth`: once resolved: the depth image the vectors were tested against.
- `d`: the listed draws.
- `e`, `ne`: while listing one per draw (`tag`: its index); once resolved the kept ones, numbered (`skin_number()`),
  `ne` of them.
- `bytes`: the blocks' bytes.
- `block_at`: by address and size: index + 1.
- `seen`: listed draws by mesh, palette, camera block and depth image.
- `lit`: once resolved: the meshes with a kept draw into `scene_depth`.

`SK`:

- `cur`: `f[cur]` is the list being made, `f[cur ^ 1]` the last one resolved.
- `resolved`: the list being made was resolved (its draws carry their verdicts).
- `pending`: its draws not counted yet.

### `skin_has()`, `skin_put()`

A hash set of `SKIN_SLOTS` words (0: empty). `skin_has()`: whether v is in it; `skin_put()`: adds it.

### `skin_joint_format()`

An unsigned integer vertex format: what joint indices are stored as.

### `skin_pick()`

Whether the draw being replayed is a skinned scene draw (the rule above; the caller has applied the scene filter). Reads
the draw's own state only: the chunks of a parallel group ask at once.

- Only with the setting on and an upscaler, or frame generation alone, at work.

### `skin_pick_left()`

`mvobj_pick()` left a draw `skin_pick()` took: counted, 0.

### `skin_begin()`

Starts the frame's list (`mo_begin()`): the draws of the one before it that nothing counted were never recorded.

### `skin_block()`

The frame's block for a uniform binding, made at its first draw: -1 when it is not in the snapshot, -2 when the table is
full.

- Asked of the snapshot for every draw: a block the submission itself writes between two draws is dropped from it there,
  and the bytes kept for the first draw are not the second one's.

### `skin_sharp()`

Whether a bone of the palette block c of this frame turned sharply against the palette block l of last frame
(`pose_matrix_sharp()`); kept for the pieces that share the palette.

### `skin_try()`

A skinned draw `mvobj_pick()` took, before it goes onto the frame's list (the caller holds the motion-list lock): its
blocks, key and place in the stream into `*sd` and `*se`, and into `*guard` whether it is checked against last frame's
depth. 0: left to the camera, counted. vs: the last-frame variant, banks: the banks its position reads.

- A draw after the frame's vectors were made (`skin_resolve()` made that list last frame's, and f is not begun).
- The mesh: the vertex buffers the shader's inputs read (a binding no input reads keeps an earlier draw's) and the
  indices.
- The same mesh with the same palette in the same view: a second pass over it, or the draw submitted twice.
- The pre-pass draw (it writes depth, into another image than the vectors are tested against) of a mesh whose
  light-target draw carried the vector last frame.

### `skin_commit()`

The draw `skin_try()` prepared is on the frame's list as `MQ.d[draw]`.

### `skin_refuse()`

Takes a listed draw out of the frame's list, counted.

### `skin_twin_cmp()`

By mesh and palette, the draw into the scene depth first, then by place in the stream. `skin_sort_frame` is the frame
whose draws the entries name.

### `skin_resolve()`

The frame's list is complete and its motion vectors are about to be made, for the scene camera block cam, tested against
the depth image depth of w x h (`vk_fg_pose_prepare()`, on the thread that records them; ex: its submission). Decides
for each listed skinned draw whether it continues a draw of last frame (the steps above), fills the blocks of those that
do with last frame's bytes, takes the others out of the list, and makes this frame's list the one the next frame is
paired with.

- Only the list of this submission: its blocks' scratch memory lives as long as the submission.

### `skin_recorded()`

A skinned draw `vk_mvobj_execute()` records (guarded: checked against last frame's depth).

### `skin_status()`

The skinned draws' part of the motion status line (`pose_status()`): since the start, every draw `skin_pick()` took, the
ones drawn with their own motion, and the others by why they kept the camera's.

## `runtime/vk/vk_dlssg.h`

What `runtime/vk/vk_dlssg.c` promises its callers: DLSS Frame Generation through NVIDIA Streamline and
`nx2navi_dlssg.dll` ([dlssg.md](dlssg.md)).

- `vk_dlssg_wanted()`: `NX2SARIA_FRAMEGEN` = dlss, with any upscaler or none (read once).
- `vk_dlssg_early()`: before the instance exists: loads the DLL and Streamline; 1 when its proxies are there.
- `vk_dlssg_create_instance()`: for `runtime/vk/vk_device.c`: the instance and device through Streamline's proxies when
  `vk_dlssg_early()` succeeded, else the loader's.
- `vk_dlssg_enumerate()`: through the interposer too.
- `vk_dlssg_instance_proc()`: the interposer's, or NULL.
- `vk_dlssg_interposed()`: Streamline's proxies are in use (until a device is made without them).
- `vk_dlssg_create_device()`: the device through the interposer; by the loader alone, with frame generation off for the
  run, when the card is not an NVIDIA one or the interposer fails to make it.
- `vk_dlssg_device_ready()`: after the device: checks DLSS-G and takes the swapchain hooks.
- `vk_dlssg_dpa()`: Streamline's device entry points, or NULL when it is not in use.
- `vk_dlssg_create_swapchain()` and the calls declared with it: `runtime/gfx/gfx_present.c`'s swapchain calls while
  active.
- `vk_dlssg_frame()`: each present, before its command buffer is submitted.

Frames are numbered by the game's presents (frame n ends with the n-th). Reflex's frame timing while active:

- `vk_dlssg_frame_start()`: the game starts a frame (`runtime/nvn/nvn_api_obj.c`, nvnWindowAcquireTexture): Reflex's
  sleep.
- `vk_dlssg_pace()`: the game presents (`runtime/nvn/nvn_api_obj.c`, nvnQueuePresentTexture, through
  `runtime/vk/vk_backend.c` `vk_be_frame_paced()`) with the pacer's period, 0 during a movie: 1 when Reflex holds the
  frame limit where the frame starts (`runtime/pacing/dlssg_pace.h`), so the pacer keeps only the game's ceiling.
  Atomics only, no wait; 0 while DLSS-G does not run.
- `vk_dlssg_game_present()`: the game presents it (`runtime/vk/vk_backend.c` `vk_be_present()`): counts it.
- `vk_dlssg_submitted()`: its submissions went out (`runtime/vk/vk_backend.c` `vk_be_present()`, or `replay_main()` with
  asynchronous replay): the render-submit-end marker; frame is `gfx_counters.frames` of its present, the same count.
- `vk_dlssg_present_frame()`: the present thread's next blit and present are of this frame.
- `vk_dlssg_shown()`: the frames the display got, generated ones included (present thread); 0 when off.
- `vk_dlssg_multiplier_of()`: the multiplier: the frames the display gets per rendered frame, 2, 3 or 4 (one to three
  generated), set by `NX2SARIA_FRAMEGEN_MULTIPLIER` and applied at once. What a setting value gives where the GPU and
  driver allow up to max (2 to 4): the value when it is 2, 3 or 4 ("3x" too) and at most max, else the nearest that is;
  unset or empty, 2. `*why` (may be NULL): the reason the value was not taken as it is, for the log, or NULL.
- `vk_dlssg_multiplier_max()`: the highest multiplier while DLSS-G runs: 2 to 4 from Streamline's
  `numFramesToGenerateMax`; 0 while DLSS-G does not run, or before Streamline reported its limit (the first frames; one
  frame is generated until then). `*old_dll` (may be NULL): 1 when the limit of 2 is `nx2navi_dlssg.dll`'s, a build
  older than its `nx2sl_frames_max`, not the GPU's.
- `vk_dlssg_multiplier()`: the frames the display gets per game frame now: the multiplier while generation runs, 1 while
  it is off or paused (the menu). `runtime/pacing/frame_rate.c` paces the game at the frame limit over it. Any thread.

- `vk_dlssg_status()`: why DLSS-G runs or not: `DLSSG_NOT_WANTED` (`NX2SARIA_FRAMEGEN` was not dlss at start, or no DLSS
  or FSR 3.1 upscaling), `DLSSG_NO_FILES` (`nx2navi_dlssg.dll`, Streamline or its swapchain hooks missing or failing),
  `DLSSG_UNAVAILABLE` (Streamline loaded but DLSS-G or a required plugin function was unavailable).

- `vk_dlssg_why()`: why DLSS-G is unavailable, where known: 1 when the card in use has none (another vendor's card, or
  Streamline's support check found no adapter with it: an RTX 30 or older); else 0, with Streamline's result for another
  reason in `*sl_why` when it gave one (NULL when it did not, or with an `nx2navi_dlssg.dll` older than
  `nx2sl_support`).
- `vk_dlssg_gpu_may()`: whether the card in use can run DLSS Frame Generation at all, known before Streamline or NGX
  looks: 0 when it cannot (another vendor's card, or an NVIDIA GPU older than the RTX 40 series), 1 when it may
  (Streamline's or NGX's own check decides). For framegen = dlss set while the game runs, which starts only at the next
  start (`runtime/vk/vk_dlaa.c` `fg_off_why()`).

## `runtime/vk/vk_dlssg_device.h`

DLSS Frame Generation and the card the device is made on (`runtime/vk/vk_device.c` `pick_device()`,
`runtime/vk/vk_dlssg.c` `vk_dlssg_create_device()`). Header only, so that `tests/dlssg_device_check.c` checks both
decisions without a device.

With frame generation on, the instance and the device are made through Streamline's interposer, whose `vkCreateDevice`
asks the card for NVIDIA's extensions (`VK_NV_low_latency2` among them) and fails without them
(`VK_ERROR_EXTENSION_NOT_PRESENT`, -7). On a laptop with an NVIDIA card and Intel graphics, framegen = dlss and
`gpu_index` on the Intel one, that was the only attempt, and every start ended with "no usable Vulkan device" (exit code
70) until `nx2navi.ini` was edited by hand.

### `dlssg_card_skipped()`

For `pick_device()`: a card the player chose (`gpu_index`, `gpu_name`) counts as not usable while the interposer is in
use and the card is of another vendor, so the best usable card is taken instead, as for a chosen card that is gone.
Without a choice nothing is skipped: the pass takes what there is, and `dlssg_device_create()` makes the device.

`DlssgCreate`: a creation attempt: 0 (`VK_SUCCESS`) or the `VkResult` it failed with.

`DLSSG_DEV_*`: how the device was made, or tried: by the loader with the interposer not in use (`DLSSG_DEV_LOADER`);
through the interposer (`DLSSG_DEV_INTERPOSER`); by the loader because the card is not an NVIDIA one
(`DLSSG_DEV_NOT_NVIDIA`: the interposer is not asked); by the loader after the interposer refused (`DLSSG_DEV_REFUSED`:
`*refused` holds its result). In the last two frame generation is off for the run.

### `dlssg_device_create()`

Returns the result of the attempt that counts: the interposer's when it made the device, else the loader's.

## `runtime/vk/vk_dlssg.c`

DLSS Frame Generation (`NX2SARIA_FRAMEGEN` = dlss; applies after a restart) through `nx2navi_dlssg.dll`
(`runtime/dlssg/nx2_sl.cpp`), which drives NVIDIA Streamline. Like AMD's frame generation (`runtime/vk/vk_fg.cpp`) it
takes the depth, motion vectors and camera `runtime/vk/vk_dlaa.c` makes for DLSS or FSR 3.1 upscaling, or without an
upscaler for frame generation alone (`runtime/vk/vk_dlaa.c` `fg_only_frame()`: the same pass at the scene's size, no
jitter); it needs an RTX 40 or 50 GPU.

The DLL and Streamline (`sl.interposer.dll` and its plugins, `nvngx_dlssg.dll`) sit next to the game. The instance and
device are created through Streamline's proxies (`runtime/vk/vk_device.c`), so it can add the extensions, features and
queues DLSS-G needs; the present path calls Streamline's swapchain functions (`runtime/gfx/gfx_present.c`), and every
present records the frame's constants and tags first (`vk_dlssg_frame()`). Generation pauses while the menu is open.

Multi frame generation: `NX2SARIA_FRAMEGEN_MULTIPLIER` (2, 3 or 4, live) is how many frames the display gets per
rendered frame, so one, two or three generated ones. Streamline says how many this GPU and driver can generate
(`numFramesToGenerateMax`: one where only 2x runs, three on an RTX 50); a higher setting is clamped to it with a log
line. The frame limit counts the frames the game draws and the generated ones come on top (`runtime/pacing/frame_rate.c`
`limit_game_hz()`): 60 at 4x is 60 drawn and 240 shown. Only a limit of display is divided by the multiplier in effect
(`vk_dlssg_multiplier()`), since a frame past the monitor's refresh is never seen.

DLSS-G needs Reflex, whose low-latency mode paces the game (`NX2SARIA_REFLEX`, default 1; 0 off, 2 on with boost): the
game's frame sleeps in Reflex where it starts (its acquire of a window image), and each frame carries the latency
markers from that start to the host present, all with the frame's number, the count of the game's presents up to it. The
mode is the swapchain's, so it is set again after each one (`vk_dlssg_create_swapchain()`).

One limiter. Reflex's sleep also holds the frame limit (its frame limit, the driver's minimum interval), and the game's
own pacer then keeps only the game's ceiling (`vk_dlssg_pace()`, `runtime/nvn/nvn_api_obj.c`, nvnQueuePresentTexture):
the pacer's wait came after the picture was drawn, so the pad was read up to a period before the picture left, and
Reflex's sleep at the frame's start already waited for the GPU. The pacer keeps the limit while a movie plays, before
the first host present, while none went out for `DLSSG_HOLD_IDLE_NS`, with `NX2SARIA_REFLEX_LIMIT=0` (live), a DLL older
than `nx2sl_reflex2` or a limit Streamline refused, and for the rest of the run once the frames show Reflex does not
hold it (`runtime/pacing/dlssg_pace.h`). The guest thread that presents decides and the present thread hands the limit
to Streamline before its present (`vk_dlssg_frame()`, under the present lock, which keeps Streamline's options calls on
one thread at a time); the pacer stands aside only once Streamline took that limit.

The state, `S`:

- `fail`: `DLSSG_NO_FILES` or `DLSSG_UNAVAILABLE` when it was wanted and does not run (`vk_dlssg_status()`).
- `mode`, `pending`: generation as the driver has it, and the frames since the host asked for the other state.
- `shown_mult`: `vk_dlssg_multiplier()`: the frames per game frame as last handed to the DLL (1 while off).
- `frames_max`: NULL with an older DLL, which generates one frame.
- `support`: NULL with an older DLL: the reason DLSS-G is unavailable is not known here.
- `no_gpu`: the card in use has no DLSS-G: another vendor's, or Streamline's check says so.
- `sl_why`: else Streamline's support result, when it gave one (`vk_dlssg_why()`).
- `reflex2`: NULL with an older DLL: Reflex holds no frame limit, the pacer keeps it.
- `game`: the game's presents (`vk_dlssg_game_present()`).
- `cur`: the frame the present thread blits and presents next (`vk_dlssg_present_frame()`).
- `started`: the last frame `vk_dlssg_frame_start()` started.
- `hosted_ns`: `kern_now_ns` after the last host present.
- `limit_want`, `limit_set`, `limit_refused`: Reflex's frame limit, in microseconds (0 none). `limit_want`: what
  `vk_dlssg_pace()` asks for (the presenting guest thread writes it); `limit_set`: what Streamline last took (written
  under the present lock, `reflex_apply()`); `limit_refused`: Streamline refused a limit, so the pacer keeps it for the
  run. Atomics.
- `pace`: the presenting guest thread's (`vk_dlssg_pace()`).
- `pace_said`: who held the limit as last logged: 1 Reflex, 0 the pacer, -1 not yet.
- `n_hold`, `hold_ns`, `n_unheld`: the frame starts Reflex's sleep held, and their time, since the last statistics line
  (`stats()`); atomics.

### `reflex_apply()`

Reflex's mode and frame limit to Streamline: under the present lock (`vk_dlssg_frame()`, `vk_dlssg_create_swapchain()`),
or once at the device's creation before any present. Streamline's `slReflexSetOptions` is not thread safe, and every
caller is one of these. A limit Streamline refuses leaves the limit to the pacer for the run.

### `limit_allowed()`

Reflex may hold the frame limit: `NX2SARIA_REFLEX_LIMIT` (default 1, live; 0 leaves the limit to the game's own limiter,
as before Reflex's frame limit was used), and a DLL with `nx2sl_reflex2` whose limit Streamline did not refuse. The
setting is parsed again when a setting changed (`cfg_version`). Only the presenting guest thread calls it.

### `vk_dlssg_wanted()`

With any upscaler or none: `runtime/vk/vk_dlaa.c` makes the inputs either way (`vk_dlaa_frame()` says which, at the
start and at each change).

### `vk_dlssg_early()`

Three entry points of the DLL are optional, for an older build of it: `nx2sl_support` (without it the reason DLSS-G is
unavailable is not known), `nx2sl_frames_max` (2x only without) and `nx2sl_reflex2` (the pacer keeps the limit).

### `vk_dlssg_instance_proc()`

An instance function through the interposer while Streamline is loaded, else NULL (the caller takes the loader's). The
window surface goes through it: Streamline learns the window a swapchain shows from `vkCreateWin32SurfaceKHR`, and
without that its swapchain hook found "no window corresponding to this application" and generated no frames.

### `vk_dlssg_enumerate()`

The interposer's `vkCreateDevice` finds the device's instance in a map its own `vkEnumeratePhysicalDevices` fills; a
physical device enumerated through the loader alone left it a null instance, so the plugins got an empty instance table
and `sl.common` called a null `GetPhysicalDeviceMemoryProperties` while it set up. Enumerate through it as well.

### `give_up()`

Frame generation is off for the run from here on: every later call goes to the loader alone, as when Streamline's files
are missing, and the settings menu says it is unavailable (`vk_dlssg_status()`).

### `create_interposed()`

The interposer's `vkCreateDevice` asks the device for NVIDIA's extensions (`VK_NV_low_latency2` among them) and fails
without them (`VK_ERROR_EXTENSION_NOT_PRESENT`). On a laptop with an NVIDIA card and Intel graphics, frame generation
set to DLSS and the Graphics card setting on the Intel one, that ended every start with "no usable Vulkan device" (exit
code 70) until `nx2navi.ini` was edited by hand: four reports on 0.10.1. A device of another vendor is now made by the
loader, without frame generation, and so is one the interposer refuses for any other reason. The order of the attempts
is `runtime/vk/vk_dlssg_device.h` `dlssg_device_create()` (`tests/dlssg_device_check.c`).

### `vk_dlssg_device_ready()`

Reflex's mode is set here without a limit: there is no swapchain and no present yet.

### `vk_dlssg_gpu_may()`

NVIDIA numbers its PCI device IDs by architecture: Ada (the RTX 40 series, the first with DLSS Frame Generation) starts
at 0x2600, and every earlier one is below it (the RTX 3050 Laptop is 0x25ac); Blackwell is above.

Before the device is known the answer is that it may.

### `gpu_multiplier_max()`

Streamline's limit as a multiplier: 2 to 4, or 0 while it has reported none (the first frames). An older DLL generates
one frame: 2.

### `multiplier_wanted()`

The multiplier the setting gives here (present thread), clamped to the GPU's limit; one log line each time the value or
the limit changes and the value is not taken as it is. Until Streamline reports its limit, one generated frame, and no
log line about the GPU yet.

### `vk_dlssg_dpa()`

For `runtime/vk/vk_device.c`: while Streamline is loaded, every device entry point the runtime fetches goes through its
interposer, so its plugins see the calls they hook (Reflex's frame timing and DLSS-G's presents). NULL keeps the
loader's.

### `vk_dlssg_create_swapchain()`

Called by `runtime/gfx/gfx_present.c` `create_swapchain()`, under the present and queue locks: the new swapchain has
neither the mode nor the limit, so both go to Streamline again, the limit as the presenting guest thread last asked for
it. One line for each of the first 16 swapchains says the limit sent and the one Streamline took:
`runtime/dlssg/nx2_sl.cpp` `nx2sl_reflex2()` logs only its first calls, and the start (the device, the first swapchain,
the first hand-over, a movie) uses them up.

### `stats()`

One line after the first 600 blits of a game frame and every 3600 after that (600 with `NX2SARIA_TRACE=perf`), on the
present thread outside the queue lock (`vk_dlssg_frame()`): who holds the frame limit, and how many frame starts
Reflex's sleep held and for how long.

### `FG_SETTLE_FRAMES`

Turning generation on or off is an `slDLSSGSetOptions` call and Streamline's own rebuild, and it can be seen: around a
menu, a fade or a swapchain change the depth and motion-vector views come and go for a frame or two. Following them
frame by frame switched generation off and on dozens of times a second (a run logged 32 changes, in pairs, a frame
apart), so a change waits until the new state has held for `FG_SETTLE_FRAMES` frames.

### `vk_dlssg_frame_start()`

Reflex holds the frame's start until the time the driver computes from the measured render and queue times, and no
sooner than its frame limit after the frame before (`vk_dlssg_pace()`), so the game's CPU work ends about when the GPU
is free for it; then the frame's first markers. Once a frame: the game may acquire again before it presents. No hold
while no host present went out for `DLSSG_HOLD_IDLE_NS` (the window minimized: `runtime/gfx/gfx_present.c` skips the
host presents while the game runs on): the driver then has no present to time the frame by, and the sleep would wait out
Streamline's half-second timeout each frame.\n\nThe guest thread gave its core up for the acquire (`NVN_BLOCKING`). The
sleep counts as the pacer's wait (`PERF_PACE`) only while Reflex holds a frame limit (`limit_set`), whose place it then
takes, on whichever guest thread acquires (`perf_end_any()`), as the acquire's own waits do. Without one (`reflex_limit`
0, an older DLL, a movie, a refused limit, frames that showed Reflex does not hold it) the sleep is only the driver's
wait for the GPU while the pacer waits as well, so it stays out of `PERF_PACE`, as before Reflex held a frame limit:
counted there, GPU-bound frames read as paced in the perf windows.

- The time of the last host present is read before the clock, in this order, so that the clock's value is never before
  it.

### `pace_say()`

Who holds the frame limit, as a log line when it changes (the first 40). `why`: the reason the pacer holds it.

### `vk_dlssg_pace()`

The limit wanted is stored for the present thread, which hands it over (`vk_dlssg_frame()`). A limit Streamline has yet
to take is no change: the present thread hands it over next.

### `vk_dlssg_frame()`

- A change of generation waits (`FG_SETTLE_FRAMES`): the state the driver has is held until the new one stays.
- `shown_mult` is the pacer's divisor (`runtime/pacing/frame_rate.c`).
- Reflex's frame limit as the presenting guest thread last asked for it (`vk_dlssg_pace()`), before this present.
- The frame's own near and far planes (`runtime/vk/vk_dlaa.c` `proj_planes()`); `nx2navi_dlssg.dll` tells Streamline the
  depth is not inverted, which holds for every projection the game has shown (0 near, 1 far).
- Render pixels: the scene's size, which is the output's or smaller (an upscaling mode, or the game's own scale without
  an upscaler); Streamline takes the depth and the vectors at their own size and the output at ext.
- The frames to generate between two rendered ones: the multiplier less the rendered one, 0 off.

### `vk_dlssg_release()`

A device made again starts without a limit.

## `runtime/vk/vk_hdr.c`

HDR output (`NX2SARIA_HDR=1`): an HDR10 swapchain (Rec. 2020 primaries, PQ) and a game picture tone mapped to the
display's range (`runtime/shaders/hdr_tonemap.frag` says how). Settings: `NX2SARIA_HDR`, on and off following the menu
during a run (`vk_hdr_wanted()`), the game's own HDR (`native`, the menu's other HDR) from the start only; the nits
`NX2SARIA_HDR_PEAK_NITS` (the display's peak), `NX2SARIA_HDR_GAME_NITS` (the picture's white, "paper white") and
`NX2SARIA_HDR_UI_NITS` (the HUD's and this menu's white); `NX2SARIA_HDR_TONEMAP` with what each tone mapper takes
(`NX2SARIA_HDR_MIDGRAY`, `NX2SARIA_HDR_MATCH_SLOPE`, `NX2SARIA_HDR_TONE_SLOPE` and `NX2SARIA_HDR_HUE_CLIP`, or
`NX2SARIA_HDR_HIGHLIGHT_SAT` .. `NX2SARIA_HDR_GRADE`), the grading sliders `NX2SARIA_HDR_EXPOSURE` ..
`NX2SARIA_HDR_SATURATION` of both and `NX2SARIA_HDR_GAMMA`, the display the present shows the picture as; all read when
the settings change (`vk_hdr_support()`). Left at their defaults they give one look: the observer-response mapper
anchored to the game's mid grey and tone slope, half of the hue bend of the game's clamp, no grading, shown as a 2.2
display would. With `NX2SARIA_HDR=0` the swapchain is SDR, the spliced tonemap gives the game's own SDR picture
(`vk_hdr_support()`, `p0.w` = -1) and the present is what it was.

The game's picture ([architecture.md](../architecture.md), "HDR"). The game tonemaps its scene into an r11g11b10f
target, upscales it and draws the HUD into a second one (float, nothing in these passes clamps), and one last draw
copies that into the 8-bit sRGB window texture it presents. So:

1. `vk_hdr_shader_swap()`, on each fragment shader as it loads (in memory only; the cache keeps the translator's):
   1.1.1's tonemap ends in the fitted ACES curve and a clamp to 0..1 per channel. `tonemap_splice()` links
   `runtime/shaders/hdr_tonemap.frag` `nx_tonemap()` into that shader and stores its result in place of the three
   clamped channels: the scene mapped onto the display by the tone mapper chosen (that file says how each does it), from
   the values the curve takes and the scene before the game's exposure gain, with the settings in five vec4 the support
   buffer gains at 1440. Recognised by the curve's constants, not by a hash, so another translator still finds it. The
   game's FSR 1 upscale after it would clamp the picture to 1 again; `fsr_unclamp()` drops that clamp in its SPIR-V.
2. `vk_hdr_draw()` and `vk_hdr_present_source()`: a draw into a presented window texture that samples one float texture
   of its size is that copy; the present then reads the float texture instead of the window texture. Any other last draw
   into it leaves the window texture, decoded from sRGB: menus and movies stay SDR at the HUD's white. The present is
   queued before the next frame's work (`runtime/vk/vk_backend.c`), so the float target is read before the game draws
   into it again.
3. `vk_hdr_encode()`: on the present, the crop rectangle goes to PQ code values in an RGBA16F image
   (`runtime/shaders/hdr_encode.comp`: gamma correction, gamut compression, the HUD's white and the peak) that FSR 1 or
   the blit scale into the swapchain. `vk_hdr_ui_*`: the in-game menu draws into an RGBA8 image and
   `runtime/shaders/hdr_ui.frag` blends it onto the swapchain at the HUD's white.

### Settings

What the start decides for the whole run (`hdr_start()`, first asked when the Vulkan instance is made), in `g_armed` and
`g_native`:

- Armed: the HDR path is set up, so the menu can switch HDR on and off without a restart: the instance and device get
  the HDR extensions (`runtime/vk/vk_device.c`), the game's tonemap is spliced as its shaders load
  (`vk_hdr_shader_swap()`) and every draw's support buffer carries the settings (`runtime/vk/vk_exec_draw.c`). While the
  swapchain is SDR the spliced tonemap returns the game's own SDR result. Always, unless `NX2SARIA_HDR_LIVE` = 0 with
  `NX2SARIA_HDR` = 0 (the path left out as it was before the setting followed the menu: HDR then needs a restart) or a
  probe turned it off (`vk_hdr_off()`).
- Native: `NX2SARIA_HDR` = native, the game's own HDR. The display is reported as HDR10 (`runtime/hle/ov_gfx.c`,
  QueryDisplayHdrStatus, with the peak and paper white of the settings), and the game then renders its HDR picture
  itself and presents it as PQ-encoded Rec. 2020 in A2B10G10R10 (format 64), which goes to the HDR10 swapchain as it is:
  no tonemap splice, no float picture, no encode (pictures still presented in 8 bits, before the switch, are encoded as
  for `NX2SARIA_HDR=1`). The game asks once, so a change to or from native waits for a restart
  (`vk_hdr_needs_restart()`).

### `hdr_start()`

`g_armed` is written last: it is the mark that both values are read.

### `vk_hdr_wanted()`

HDR is on now: the setting as the menu has it, read again when the settings change. The swapchain follows at the next
present (`runtime/gfx/gfx_present.c` `window_settings_live()` makes it again) and `vk_hdr_active()` then says what it
got. A run that started with the game's own HDR keeps it: the game goes on presenting its PQ picture whatever the menu
says.

### `vk_hdr_needs_restart()`, `vk_hdr_restart_for()`

The menu's HDR row: what is set takes a restart. To or from the game's own HDR, and switching HDR on in a run that
started without the HDR path (`NX2SARIA_HDR_LIVE` = 0). `vk_hdr_restart_for()`: the same for a value not set yet (the
menu's draft holds HDR until Apply).

### The settings' snapshot

The settings, read again when they change. The replay (every draw's support buffer) and the present thread read them: an
update fills the snapshot not in use and then publishes it.

The game's picture is graded for a display of gamma about 2.2, and its sRGB encode is linear near black: shown as that
encode decodes, its darkest tones sit up to a hundred times above where such a display puts them (a scene value of
0.001, 0.08 nits at a white of 350 nits against 0.001), the floor of a dark scene lifted to grey. So the present shows
the picture as a 2.2 display would (`runtime/shaders/hdr_encode.comp`, `runtime/shaders/hdr_ui.frag`: the power 2.2 of
the sRGB code), and the tonemap aims its peak below the display's by that correction's inverse
(`runtime/shaders/hdr_tonemap.frag` `peak_before_gamma()`). That holds for every tone mapper and is the default of
`NX2SARIA_HDR_GAMMA`, which can make it 2.4 or switch it off (the picture as its sRGB code decodes, the dark tones
lighter). The game's own HDR does not take the setting: what the present still encodes there (the menu, the 8-bit
pictures before the game's switch) is shown as a 2.2 display would.

`HDR_GAMMA_22` (1.0) is the present's correction as the shaders number it: 0 none, 1: 2.2, 2: 2.4.

`Nits`:

- `peak`, `game`, `ui`: nits.
- `gamma`: the present's correction, as `HDR_GAMMA_22` numbers it (`runtime/shaders/hdr_encode.comp`,
  `runtime/shaders/hdr_ui.frag`).
- `tm`: the support buffer's five vec4 (`runtime/shaders/hdr_tonemap.frag` p0..p4).

### `nits()`

- The tone mapper as `runtime/shaders/hdr_tonemap.frag` numbers it: the game's curve, the luminance mapper, the
  observer-response mapper (the default, and what any other value gives).
- The sliders' 0..100 with 50 unchanged become factors around 1; blowout, hue, grade and hue clip are fractions.
  Divided, not multiplied by a rounded reciprocal: 50 and 100 % come out as exactly 1, which the shader compares for to
  leave a grading step out.

### `vk_hdr_support()`

Without an HDR10 swapchain (yet), `p0.w` is -1: the game's own SDR picture.

### `name_is()`

Compares the operands of an OpName after the id.

### `is_clamp()`

A 0..1 clamp: translators write FClamp, or NClamp (NaN gives the low bound, as the GPU's saturate does).

### `fsr_unclamp()`

The game's FSR 1 pass after the tonemap (some frames take it in place of the plain pass, at 1:1 as well as when it
scales up; what picks it is not known yet) ends with clamp(colour, 0.0, 1.0) per channel, which cuts the HDR highlights
back to the HUD's white for as long as the game takes it. Its own 0..1 clamps of the filter weights each feed a float
bit trick (OpBitcast), so only the clamps whose result is stored to the shader's output change, to max(x, 0.0): one word
shorter, so the words after it move down and the module shrinks. Recognised by FSR's two reciprocal constants
(0x7ef07ebb, 0x7ef19fff) and its one texture `fp_t_tcb_8`. Returns the clamps changed, -1 for that shader with no output
clamp found (a translator that stores the colour another way), 0 for any other shader.

- `out` maps an id to 1, a pointer into an Output variable, or 2, a value stored through one.
- The module is rewritten in place: the write position is never past the read position, as instructions only shrink.
- The operands of the extended instruction are result type, result, set, instruction, x, min, max.

### `spv_literal()`

Which words of an instruction are literals rather than ids, for the opcodes the tonemap library uses (word j, 1 is the
first after the opcode word). An opcode it does not know makes the splice give up (-1).

Among those it gives up on are OpSwitch, OpLine, OpNoLine and OpCopyMemory (63).

### `put_mapped()`

One library instruction with its ids mapped (`map[id]`, new ones already assigned); 0 when an id has no mapping.

### `SpvView`

What `tonemap_splice()` knows of the game's module while it looks for the exposure: where each result is defined
(`def`), the class of each variable (`cls`: 1 output, 2 uniform, 3 function), and for a function variable the value its
stores gave it and how many there were (`sval`, `scnt`).

### `spv_stored()`

The value behind id when id loads a function variable that is stored once (the translator keeps every value in one); id
itself otherwise.

### `spv_uniform_load()`

Whether id loads a member of a uniform block.

### `spv_same_source()`

Whether a and b are the same value read twice: one id, two loads of one variable, or two loads through access chains of
one base and the same indices (the translator loads a uniform anew at each use).

### `spv_common_factor()`

Whether the three values are products with one factor in common: 1 with the factor (the first value's operand) and what
each is multiplied with.

### `spv_common_fma()`

Whether the three values are fma(a, f, t) with one f for the three: 1 with f, and each value's a and t.

### `spv_exposure()`

The game's exposure in the three x it feeds its curve, and those values without local exposure ("exposed"; each plus add
where add is set). The shapes:

- x = scene \* gain, one gain for the three: a uniform in the scene's tonemap (its constants, element 3 .z), a value the
  vertex shader passes in the shaders that draw into the tonemapped picture with the curve of their own. The exposed
  values are x.
- x = (scene \* gain) \* local exposure, the gameplay shader: the local exposure computed from a grid, the gain a
  uniform under it. The exposed values are the inner products.
- x = fma(scene, gain, colour), a colour added per channel after the exposure, the gain a uniform: x again.
- x = fma(scene \* gain, local exposure, colour): the inner products, plus the colours.

Returns 1 when one of them is found.

### `spv_top_level()`

The instruction at pos, between the function's first label and end, is in a block outside every selection and loop: such
a block runs on every path and before the blocks after it. Constructs nest between the instruction that names their
merge block and that block's label.

### `tonemap_splice()`

The fitted ACES curve the game ends its tonemap with: FClamp/NClamp(FMul(FMul(x, Fma(x, 2.51, 0.03)), rcp), 0, 1) per
channel, each result stored to a function variable that the output store of its channel loads. `tonemap_splice()` keeps
everything the game computes and only changes what the three colour channels store: `runtime/shaders/hdr_tonemap.frag`
`nx_tonemap()`, called just before the first of those stores, of the three x, of those values without local exposure
with the exposure gain in them (`spv_exposure()`; x and a gain of 1 when it finds none), and of the settings from the
`HDR_SETS` vec4 the support buffer gains at 1440 (`runtime/vk/vk_exec_draw.c` `support_fill()` writes them; the
translator's SupportBuffer ends there). The library's types are matched to the game's where the same (SPIR-V allows one
declaration of each), its constants and functions appended with new ids. Gives up (0) on any shape it does not expect,
and the shader stays as it was.

`HDR_SETS` is 5 and `HDR_PARAMS` 7: the function is `nx_tonemap(vec3 x, vec4 scene, vec4 settings[HDR_SETS])`.

The steps:

- The tables by id: `def`, the word index of a result's instruction; `cval`, an integer constant's value + 1; `cls`, a
  variable's class (1 output, 2 uniform, 3 function); `sval`, the value last stored to a variable; `scnt`, its stores,
  counted to 2.
- The support buffer is the struct whose member 10 is at 1424.
- Where each result is defined: the globals the later checks look at, and inside the function every instruction that has
  a result (its second word; the first definition stands, so a word of an instruction without one, taken for a result
  here, cannot replace a real one). Left out there, as instructions without a result: the stores, labels, merges and
  branches, OpCopyMemory (63), OpImageWrite (99), OpAtomicStore (228) and OpReturnValue (254).
- The support buffer's block and a Uniform vec4 pointer are looked for among the globals.
- The clamp sites and their x: the clamp's operand is FMul(num, rcp), and num is FMul(x, Fma(x, 2.51, 0.03)); the
  clamp's result is stored to a variable (OpStore var, result).
- The output stores of channels 0..2: OpStore (AccessChain out, c), value, with a load of a site's variable between. A
  channel stored twice gives up.
- The insertion point is the first of those stores; the span from it to the last must be one block.
- The exposed values and the gain: usable at the call when defined in a block outside every selection and loop before
  it, or in the call's own. A value with nothing added is passed as it is.
- The library: its ids mapped into the game's id space, in three outputs: the library's globals, its functions, the
  result.
- A library type takes the game's own declaration of it (any but a struct).
- A library variable that is neither an Input nor an Output is a global its functions could use, which gives up
  (`lib_fail`); main's inputs and output are left behind with main.
- Names, decorations (the inputs' locations), the entry point and the memory model stay the game's.
- `nx_tonemap`'s function type is (vec3, vec4 x (1 + `HDR_SETS`)) with its parameters by pointer, as the compiler writes
  them; the library's vec4 must be the support buffer's.
- The library's functions, all but its main. First a new id for every result in them (calls may refer ahead): OpLabel's
  is its first word, any other instruction's with a result its second; the stores, the merges, the branches (OpBranch
  249 too), OpKill (252), OpReturn (253), OpReturnValue (254), OpUnreachable (255) and OpFunctionEnd have none.
- The result: the game's module with the support block grown, the library's globals before the first function, the call
  in main, the three stores changed, and the library's functions at the end. The support block gains `HDR_SETS` vec4 at
  1440.

### `vk_hdr_shader_swap()`

Armed, not wanted: a shader loaded while HDR is off must be ready for the menu switching it on.

### The picture the window texture is copied from

- `vk_hdr_track`: `runtime/vk/vk_exec_draw.c` calls `vk_hdr_draw()` while it is set: set at the first present with
  `NX2SARIA_HDR=1`.
- `g_active`: the swapchain is HDR10 (`runtime/gfx/gfx_present.c` calls `vk_hdr_set_active()`).

- `recs`: per presented window image: what the last draw copied, and where in the stream (`vk_exec_stream_pos()`).

### `vk_hdr_draw()`

- An unlocked first look: most draws go elsewhere (a stale slot only costs the lock that follows).
- The last such draw of the stream decides, as in the replay, also when replay workers record draws at once.
- The source is the one plain 2D texture the draw samples.

### `vk_hdr_present_source()`

The record is kept until the next draw into the window texture: a present again without a new copy shows the same
picture.

### `vk_hdr_image_gone()`

A texture image went away: forget it as window or source.

No records exist before the first HDR present (`vk_hdr_present_source()`), so nothing is locked per image until then.

### `vk_hdr_surface_format()`

While `NX2SARIA_HDR` is on (`vk_hdr_wanted()`), the HDR10 format among the surface's (10-bit UNORM, ST 2084): 1 with
`*out` set, else 0 and the reason logged once.

### `vk_hdr_metadata()`

The display's peak and the picture's white for sc; again when the settings change (`runtime/gfx/gfx_present.c` calls it
on every present to an HDR10 swapchain, with new = 1 for a new swapchain).

AMD's frame generation swapchain takes it through its own call (`runtime/vk/vk_fg.cpp` `vk_fg_hdr_metadata_fn()`).

### `img_ensure()`

M at w x h in fmt; another size waits for the device first (earlier presents may still read the old image).

### The present passes' state

`H`:

- `ui`: for `ui_fmt`.
- `out`: PQ code values of the crop, RGBA16F.
- `menu`: the in-game menu, RGBA8.

### `vk_hdr_native_picture()`

The game's own HDR picture: PQ codes in A2B10G10R10, the format of NVN's RGB10A2 (`runtime/vk/vk_formats.c`); no guest
format becomes A2R10G10B10.

### `vk_hdr_encode()`

- Native HDR: the game's picture holds PQ codes already; the present scales it into the swapchain as it is.
- An sRGB format decodes itself through the view; 8-bit UNORM holds sRGB codes the shader decodes.

### `vk_hdr_ui_target()`

The in-game menu's target: an RGBA8 image of the swapchain's size in `COLOR_ATTACHMENT_OPTIMAL` (its old content
dropped); the caller renders the menu into the view with a clear to 0.

### `ui_pipeline()`

Another swapchain format: earlier presents may still use the old pipeline, so the device is waited for first.

### `vk_hdr_ui_compose()`

The menu image onto dst (a swapchain image in `COLOR_ATTACHMENT_OPTIMAL`, view `dst_view` of format fmt).

### `vk_hdr_images_release()`

The swapchain is made without HDR (`runtime/gfx/gfx_present.c` `create_swapchain()`): the encode's picture and the
menu's image at the picture's size (28 and 14 MiB at 2560x1440, 63 and 32 MiB at 2160 lines) were kept for the process
once HDR had been on. They go now, after a wait for the presents that may still read them; HDR switched on again makes
them anew.

### `pq_nits()`

Luminance of a PQ code (0..1023 over 1) in nits.

### `vk_hdr_to_srgb8()`

10-bit PQ pixels of an HDR10 swapchain to bytes that show on a display of gamma 2.2 what the HDR display shows, the
HUD's white at 255 (menu test shots and flight thumbnails): a dark tone the HDR display lifts looks lifted here too.

- Rec. 2020 to Rec. 709 (the inverse of ITU-R BT.2087's matrix).
- A2B10G10R10: red is the low 10 bits.

### `vk_hdr_luminance_stats()`

The luminance (Rec. 2020 Y, in nits) of the 10-bit PQ pixels inside the rectangle {x, y, w, h} of a stride-wide image:
out = the darkest pixel's, the 0.1st, 1st, 50th and 99th percentiles, the brightest's, and the share of pixels under
0.01 nits. The percentiles come from a histogram of 64 bins a stop, from 2^-24 to 2^14 nits; a value is a bin's upper
edge.

- Bin 0 is exactly black.

## `runtime/vk/vk_dlaa.h`

What `runtime/vk/vk_dlaa.c` promises its callers: DLSS or FSR 3.1 on the scene, behind `NX2SARIA_DLAA` (`dlaa = 1` in
`nx2navi.ini`), and the scene's depth, motion vectors and camera for frame generation.

`DLAA_SCENE_MIN_W` (320): the narrowest scene the DLSS pass and the motion passes recognise by its D32F depth: Ultra
Performance at a 1080p output renders 640 wide, smaller windows less.

The setting and the upscaler:

- `vk_dlaa_wanted()`: the setting is on (the device may still lack DLSS).
- `vk_dlaa_fsr3()`: the setting is on and its upscaler is AMD FSR 3.1 (`NX2SARIA_TAA` = fsr3) instead of DLSS, which
  loads.
- `vk_dlaa_needs_restart()`: the menu's upscaler cannot start in this run.
- `vk_dlaa_instance_exts()`, `vk_dlaa_device_exts()`, `vk_dlaa_init()`: for `runtime/vk/vk_device.c`: NGX's instance and
  device extensions join the lists (room for max names); `vk_dlaa_init()` after the device exists brings NGX up and
  checks that DLSS is available.
- `vk_dlaa_release()`: called by `runtime/vk/vk_device.c` `destroy_all()`, once the device is idle and before the
  allocator and the device go (Linux): releases the NGX features and parameters, frees every image, buffer, pipeline and
  query pool DLAA made, resets its state for a later device and shuts NGX down for this one. Safe when NGX never came
  up, when nothing was made, and more than once. Windows: does nothing.
- `vk_dlaa_available()`: 1 when dlaa is on and NGX came up with DLSS (after `vk_dlaa_init()`): a DLSS mode's smaller
  scene is scaled up by DLSS.
- `vk_dlaa_active()`: 1 while DLSS or FSR 3.1 runs on the scene (the setting is on, the upscaler came up and has not
  failed): it scales the scene itself, and `vk_dlaa_upscaler()` puts its picture over the stand-in's. The thread that
  records the frame asks.
- `vk_dlaa_vram()`: the video memory DLSS reports for its features (NGX's statistics), for the log; -1 while unknown.
- `vk_dlaa_resized_take()`: 1 once, at the present of the first frame whose scene has a new size after a setting
  changed.

DLSS Frame Generation through NGX itself (the `vk_ngxfg_*` functions, for `runtime/gfx/gfx_present.c`'s presenter):
`NX2SARIA_FRAMEGEN` = dlss where Streamline does not run (every build but the Windows one, where these all answer 0). A
`VkNgxImage` is in GENERAL.

- `vk_ngxfg_wanted()`: this run can generate frames through NGX (DLSS upscaling on at the start). `vk_ngxfg_on()`: and
  `NX2SARIA_FRAMEGEN` = dlss now (the menu switches it live).
- `vk_ngxfg_max()`: generated frames per rendered one the GPU allows (1 = 2x, 5 = 6x), 0 when unavailable.
- `vk_ngxfg_reset()`: the next generation starts without history (a resize, the menu).
- `vk_ngxfg_evaluate()`: records `count` generated frames into `out[]` and a copy of bb into `real`; returns how many
  were made (0: none, and `real` is not written).

What frame generation takes (`runtime/vk/vk_fg.cpp`, `runtime/vk/vk_fg4.cpp`, `runtime/vk/vk_fsr3fg.c`,
`runtime/vk/vk_dlssg.c`):

- `vk_dlaa_fg_inputs()`: the depth and motion vectors of the frame DLSS or FSR 3.1 last ran on, with its jitter and
  whether its history reset; 1 once per evaluation (0: none since the last call). The images stay valid until the next
  frame's scene draws over them, after this present. Without an upscaler, while frame generation runs, the same of the
  last frame armed for it alone (`runtime/vk/vk_dlaa.c` `fg_only_frame()`): the scene's size, a jitter of 0.
- `vk_dlaa_fg_only()`: 1 while frame generation runs without an upscaler and the frame is armed for its inputs alone
  (`fg_only_frame()`): the motion passes run as for the upscaler (`runtime/vk/vk_usnap.c`, `runtime/vk/vk_exec_draw.c`,
  `runtime/vk/vk_mvprev.c`), nothing is jittered. Read at each present.
- `vk_dlaa_fg_views()`: their views.
- `vk_dlaa_fg_camera()`: this frame's camera for DLSS Frame Generation (`runtime/vk/vk_dlssg.c`), a `VkDlaaCamera`:
  matrices for row vectors (v \* M, the rows stored in order), as Streamline takes them; 0 without a camera that frame.
  `znear` and `zfar`: the view distances the depth buffer holds as 0 and 1 (1 and 0 when inverted), from the frame's own
  projection and the device's depth mode (`runtime/vk/vk_dlaa.c` `proj_planes()`); with an infinite far plane
  (`infinite`) `zfar` is `VK_DLAA_FAR_INFINITE` (1e8), a distance no surface reaches.
- `vk_dlaa_ffx_camera()`: the same camera as FSR takes it (`runtime/vk/vk_ffx.h`); 0 without one that frame.
- `vk_dlaa_fg_hudless()`: the picture without the HUD at the size of the game's picture, in `*fmt`: RGBA8 UNORM bytes
  like it (sRGB-encoded), or RGBA16F linear with HDR on; made at each evaluation while `vk_dlaa_hudless_wanted` (frame
  generation sets it), from the upscaler's picture or, without an upscaler, from the game's own scaled picture; 1 once
  per evaluation.
- `vk_dlaa_fg_scene()`: the frame that ended last drew a scene (a light target armed, or draws into a scene-size depth):
  0 on the title, a movie, a load or a fade, which frame generation's log counts apart from gameplay
  (`runtime/vk/vk_fg_stats.h`). Any thread.

The hooks of the replay:

- `vk_dlaa_pass()`: called by `runtime/vk/vk_exec.c` `vk_exec_pass_begin()`, once the pass's targets are resolved and
  before its barrier: arms the frame on the scene's light target or, at the first quarter-width pass after it, runs DLAA
  on that target. Each present ends the frame (`vk_dlaa_frame()`).
- `vk_dlaa_retarget()`: called by `vk_exec_pass_begin()`, in every replay thread, after the pass hooks: with the
  upscaler run before the bloom to the picture's size (`NX2SARIA_DLSS_PREBLOOM`), the passes after it that draw into the
  light target or the bloom composite's target are moved to images of the picture's size (`runtime/vk/vk_dlaa.c`
  `prebloom_run()`).
- `vk_dlaa_target_scale()`: the scale from the game's viewport and scissor to such a moved pass; 0 for any other pass
  (`runtime/vk/vk_exec.c` `vk_exec_scissor()`, and `vk_dlaa_viewport()` here).
- `vk_dlaa_upscaler()`: called by `runtime/vk/vk_exec_draw.c` `untranslated_dispatch_fallback()`, after the stand-in of
  the game's own upscaler wrote its output image (in GENERAL, of format fmt and w x h): DLSS or FSR runs there on the
  armed scene and its picture goes over it.
- `vk_dlaa_draw()`, `vk_dlaa_viewport()`: called by `runtime/vk/vk_exec_draw.c` for each draw before its fixed state is
  recorded: `vk_dlaa_draw()` notes the scene camera block, and `vk_dlaa_viewport()` moves the viewport of the scene's
  draws by the frame's jitter, when the viewport is recorded.
- `vk_dlaa_pass_end()`: called by `runtime/vk/vk_exec.c` when a pass just ended: the edges of a jittered pass's targets
  are filled.
- `vk_dlaa_jitter_check()`: before each draw's fixed state: records the viewport again where its jitter changes.
- `vk_dlaa_skip_draw()`: called by `runtime/vk/vk_exec_draw.c` once a draw's fragment shader is known: 1 skips the draw
  (the game's FXAA while `NX2SARIA_FXAA` is 0, its scaling draw when DLSS wrote the scaled picture).

The motion files' functions declared here:

- `vk_mvsurvey_draw()`, `vk_mvsurvey_frame()` (`runtime/vk/vk_mvsurvey.c`, `NX2SARIA_MV_SURVEY`): notes the uniform
  blocks of each scene draw, beside `vk_dlaa_draw()`; each present compares them with the previous frames' and logs a
  summary now and then.
- `vk_mvprev_draw()` (`runtime/vk/vk_mvprev.c`): notes the scene draw's object and bone blocks.
- `vk_mvobj_execute()`: per-object motion (`runtime/vk/vk_exec_draw.c`): the moving scene draws are listed again during
  the replay with their vertex shader's last-frame variant; `runtime/vk/vk_dlaa.c` `mv_pass()` records them over the
  camera motion into `ex->cb` (and leaves every binding of the replay to be set again; draws whose camera block is not
  cam are left out). `vk_dlaa_prev_camera()`: last frame's camera block and this frame's jitter, 0 when there is no
  history.
- `vk_mvobj_frame_masks()` (`runtime/vk/vk_exec_draw.c`): the last motion list, per position and changed banks.
- `vk_mvcheck_camera()`, `vk_mvcheck_final()`, `vk_mvcheck_output()` (`runtime/vk/vk_mvcheck.c`, `NX2SARIA_MV_CHECK`,
  test): `mv_pass()` hands it the motion image after the camera pass and, after the object pass, the motion image again
  and the scene light target it ran on; after DLSS on a check frame, its output; it measures how well the vectors fit
  the picture.

## `runtime/vk/vk_dlaa.c`

DLAA: NVIDIA DLSS at the scene's own size (`NX2SARIA_DLAA`, `dlaa` in `nx2navi.ini`), the DLSS modes that scale a
smaller scene, AMD FSR 3.1 in DLSS's place, and the scene's motion vectors and camera for both and for frame generation.
`runtime/vk/vk_fg_pose.inc` and `runtime/vk/vk_skin_motion.inc` are part of this file.\n\nNGX is loaded at run time from
the driver's NGX core: on Windows `_nvngx.dll`, found through `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore`; on
Linux `libnvidia-ngx.so.1`, which the NVIDIA driver installs on the library path. NGX loads the DLSS model from the
folder of the executable: `nvngx_dlss.dll` on Windows, `libnvidia-ngx-dlss.so.<version>` on Linux. No NVIDIA SDK code or
header is compiled in: the few NGX types used are declared here from the SDK ABI and the Linux core's shutdown
interface.

The core's parameter object is a C++ class whose virtual functions are called through its vtable: the Set overloads in
slots 0-7 and the Get overloads in 8-15. MSVC (the Windows core) lays out each group of overloads in reverse declaration
order, the Itanium C++ ABI (the Linux core, built with GCC) in declaration order. `vtable_ok()` checks the layout with a
set-then-get round trip before anything relies on it.

NGX and DLAA's resources live as long as the device: `vk_dlaa_init()` brings NGX up on the new device, and
`vk_dlaa_release()` (Linux) frees everything DLAA made and shuts NGX down before the device is destroyed.

The NGX types declared here: `NgxResult` is `NVSDK_NGX_Result` (1 success, `0xBAD00000 | n` failures); `NGX_PERFQ_DLAA`
(5) is a `NVSDK_NGX_PerfQuality_Value` (0 performance, 1 balanced, 2 quality, 3 ultra performance); `NgxImageView` is
`NVSDK_NGX_ImageViewInfo_VK`; `NgxResourceVK` is `NVSDK_NGX_Resource_VK`, whose `type` is 0 for an image view and 1 for
a buffer; `PFN_Shutdown1` is `NVSDK_NGX_VULKAN_Shutdown1` as the Linux core exports it: the device and a pointer that
receives the devices NGX still serves afterwards (`vk_dlaa_release()`).

NGX's state, `N`:

- `core`: the NGX core library (an HMODULE on Windows).
- `feat_inst_req`: `NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements` (frame generation's), NULL when the core
  lacks it.
- `ngx_up`: `vk_dlaa_init()` brought NGX up: DLSS serves this run.
- `capsp`: the capability parameters (`vk_dlaa_init()`), which DLSS's statistics callback reads and fills.
- `wanted`: -1 not read yet.
- `shutdown1` (Linux teardown, `vk_dlaa_release()`): required: `core_load()` leaves DLAA off without it.
- `device`: what NGX was initialised with: set while `vk_dlaa_release()` owes it a shutdown.
- `caps_map`: GetCapabilityParameters' map: allocated for the caller, destroyed by `vk_dlaa_release()`.

`FG`: DLSS Frame Generation through NGX itself (the `vk_ngxfg_*` functions): `NX2SARIA_FRAMEGEN` = dlss where Streamline
does not run, which is every build but the Windows one.

- `max`: generated frames per rendered one this GPU allows (MultiFrameCountMax), 0: none.
- `w`, `h`: the picture the feature was created for.

### `vk_ngxfg_wanted()`

Whether this run can generate frames through NGX: DLSS upscaling on at the start, as NGX comes up only then. The
swapchain then gets the presenter's extra images and NGX is asked about frame generation; whether frames are generated
is `NX2SARIA_FRAMEGEN` = dlss, read live (`vk_ngxfg_on()`).

On Windows the answer is 0: Streamline's path serves there (`runtime/vk/vk_dlssg.c`).

### `p_seti()`

The parameter object's vtable: the SDK declares Set(u64), Set(float), Set(double), Set(u32),
Set(int), Set(ID3D11Resource \*), Set(ID3D12Resource \*), Set(void \*), then the Get overloads in the same order, then
Reset.

### `vk_dlaa_wanted()`

NGX is brought up only when DLSS is wanted at start: its device extensions can only be asked for at device creation, and
loading the driver's NGX core on every NVIDIA machine would cost start time and change the device with DLAA off. So DLSS
applies after a restart. FSR 3.1 (`runtime/vk/vk_ffx.cpp`: `amd_fidelityfx_vk.dll` on Windows, the runtime's own passes
in `runtime/vk/vk_fsr3.c` elsewhere) asks nothing of the device: `NX2SARIA_TAA` and dlaa follow the setting at once when
NGX is not up, and `vk_dlaa_frame()` loads it the first time it is used.

### `fsr3()`

The temporal upscaler behind dlaa: NVIDIA DLSS through NGX, once NGX is up, or else AMD FSR 3.1, which takes the same
inputs. The DLSS mode setting (`NX2SARIA_DLSS`) is the render scale for both.

`fsr_frame` is `fsr3()` as `vk_dlaa_init()` and then `vk_dlaa_frame()` read it: the upscaler this frame runs with.

### `vk_dlaa_fsr3()`

FSR 3.1 is the upscaler and can run (`vk_ffx_available()`): `runtime/patches/widescreen.c` shrinks the scene only for an
upscaler that runs.

### `vk_dlaa_needs_restart()`

The upscaler the menu has set cannot start in this run: DLSS when NGX was never tried (a device made without its
extensions), FSR 3.1 while NGX is up and serves the run. A DLSS this PC could not bring up is not waiting for a restart.

### `ngx_dir_from_adapter()`

The NGX core's folder when the driver has not written `NGXCore\FullPath` (seen with driver 617.42, whose key held only
DLSS override values): the core sits in the driver's own DriverStore folder, the one its Vulkan driver is in. That is
the display adapter's VulkanDriverName, where the Vulkan loader itself finds the driver.

### `core_load()`

- The driver's NGX core; the dynamic loader finds it where the driver put it (`/usr/lib`, `/usr/lib/x86_64-linux-gnu`).
- `NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements` is optional: it is frame generation's.
- Linux: without `NVSDK_NGX_VULKAN_Shutdown1` the device cannot be taken back from NGX (`vk_dlaa_release()`), and the
  process then segfaults after the Vulkan teardown (NVIDIA 615.78.08): DLAA stays off rather than run with a teardown
  known to crash. The parameter maps are destroyed there too.

`NgxDiscovery` is `NVSDK_NGX_FeatureDiscoveryInfo` with an application id (the layout of the SDK's struct, 64-bit), for
the question which instance extensions frame generation wants. `NGX_FEATURE_FRAME_GENERATION` (11) is
`NVSDK_NGX_Feature_FrameGeneration`.

### `fg_keep()`

The names NGX lists are copied: its list may not outlive the call, and the device creation keeps the pointers.

### `vk_dlaa_instance_exts()`

- Frame generation's own instance extensions (`vk_ngxfg_evaluate()`). Its device extensions are not asked for: the
  core's GetFeatureDeviceExtensionRequirements answered "not implemented" (`0xBAD00012`) with NVIDIA's 615 Linux driver,
  and NGX's general list (`vk_dlaa_device_exts()`) served the feature.
- The application id is the one `vk_dlaa_init()` gives NGX.

### `utf8_to_wide()`

UTF-8 to UTF-32 `wchar_t`, without the C library's locale (the runtime never sets one, so mbstowcs would stop at the
first non-ASCII byte). Returns 0 on malformed input or when out has no room.

### `vk_dlaa_init()`

- The executable's folder, where NGX looks for `libnvidia-ngx-dlss.so` and writes its logs, as a `wchar_t` string:
  32-bit code points here, as the Linux core expects.
- 231313132: the application id NVIDIA's DLSS samples use for builds without an NVIDIA-issued id.
- NGX holds the device from its initialisation on, also if the layout check after it turns DLAA off: `N.device` is set
  there.
- The capability parameters are allocated for the caller (the SDK's header: free it with DestroyParameters):
  `vk_dlaa_release()` does.
- MultiFrameCountMax said 3 (4x) with the 310.9.16 library on an RTX 5090, yet that library evaluated 5 generated frames
  without an error on NVIDIA's 615 driver, and NVIDIA ships up to 6x on RTX 50 cards: a GPU with multi-frame generation
  gets up to 5 (6x).

### Where DLAA runs in the game's frame

Measured (`NX2SARIA_DUMP`, frame 2100 at Lookout Landing, 2560x1440 scene): the G-buffer passes draw into five targets
with the scene depth (d32f at the scene size), one of them the R11G11B10F light target; lighting and the transparents
then draw into that light target alone, the last time with the scene depth attached; the first pass narrower than a
quarter of the scene starts the bloom chain, which reads it. After the bloom come the tonemap and post passes, then the
HUD is drawn into the same target and a last pass copies it to the 8-bit output.

So: after the G-buffer, a pass at the scene size with exactly one colour target (R11G11B10F or RGBA16F) and the D32F
depth arms the frame with that pair; the next pass at most a quarter as wide ends the scene (no jitter after it) and,
when the scene is as wide as the output, runs DLAA on the armed target first. Bloom, tonemap, post and HUD then all work
on the anti-aliased picture.

Upscaling (a scene smaller than the output: `NX2SARIA_DLSS` quality/balanced/performance, or the game's own 5/6):
measured at a 960x536 scene, 1920x1080 output, the bloom and its composite write back into the light target at the scene
size, then one full-screen draw (the FXAA shader of `vk_dlaa_skip_draw()`, which also scales) reads it into an
output-sized R11G11B10F target with no depth, and the HUD is drawn over that target at the output size. DLSS runs at the
begin of that pass: it reads the light target and its result is blitted into the pass's target, whose scaling draw is
then skipped. The HUD, drawn afterwards into the same target, is never jittered nor passed through DLSS.

The scaling draw is not always that FXAA shader: at a 1280x720 scene and 1920x1080 output (1.4.3, dlss = quality) the
game's tonemap (guest fs 7e114c4330dc212a) reads the light target and writes the output-sized target itself, over DLSS's
picture, so the jittered scene reached the screen and the whole picture shook. So from DLSS's evaluation to the frame's
end, draws in passes larger than the light target sample DLSS's output in its place (`vk_dlaa_swap_img`, applied by
`runtime/vk/vk_exec_draw.c` `tex_info()`): whichever shader scales, it scales DLSS's picture.

`OCAM_BLOCK` (0x3500): the size of the game's scene context block (`vk_dlaa_draw()`); `OCAM_BYTES` (0xb0): its rows
0-10, the view and the projection. `DLSS_SIZES` (5): scene and output sizes kept, each with its work images and a DLSS
feature (`D.sizes`).

`SIZE_STALE` (3600): a size no evaluation used for this many goes with its feature (`stale_sizes()`): DLSS's history
buffers and the work images at the output size held their VRAM after a switch of the DLSS mode, until four more sizes
had come and gone.

The state, `D`. The frame:

- `color`, `depth`: the armed pair, valid in the frame it was armed.
- `armed`, `done`, `gbuf`: this frame: armed on the light target, DLAA ran, the G-buffer was drawn.
- `ended`, `up`: this frame: the scene is drawn (jitter over), DLSS wrote the scaled output. \nA `Size`, the work images
  of a scene size and its DLSS feature (dynamic resolution flips it):\n
- `w`, `h`, `ow`, `oh`: render size, output size.
- `sharp`: FSR 3.1's second sharpening pass writes here (`fsr3_sharpen()`), at the output size.
- `mask`, `chars`: the bias mask DLSS reads, the character class mask of this and last frame (R8).
- `chars_cur`, `chars_ok`: `chars[chars_cur]` is written next; `chars_ok`: the other holds last frame's.
- `dist`: each pixel's view distance, this and last frame (R32F, `runtime/shaders/dlaa_mv.comp` `depth_out`).
- `dist_cur`, `dist_ok`: as `chars_cur` and `chars_ok`.
- `feat`: created for this size; NULL while the size runs on another's (`range_host()`) or after a release.
- `q`: the quality value feat was created with (`perf_quality()`).
- `ranged`: feat also takes the smaller render sizes from `min_w` x `min_h` up (`range_note()`).
- `used`: the count of evaluations when it last ran.
- `feat_used`: that count when `feat` last ran: behind `used` while the size runs on another's.
- `feat_fence`: the submission that last created or evaluated feat (`vk_exec_recording_fence()`). \nThe sizes and the
  history:\n\n- `last`: the size the previous DLAA frame ran with.
- `host`: the one whose feature that frame ran on: last, or the DLSS mode's (`range_host()`).
- `stepped`: this evaluation: another render size than the last one's, on the same feature.
- `ev_w`, `ev_h`, `ev_ow`, `ev_oh`: the sizes of the previous evaluation, 0 before the first.
- `steps_kept`, `steps_made`: changes of size that kept the history / that made a feature.
- `last_color`: the target DLAA ran on last frame: another one means a new history.
- `share_off`: frames left that pick the output pass without the mode's share (`vk_dlaa_pass()`, `vk_dlaa_frame()`).
- `shape_logged`: passes of another shape than the scene's turned down as the output pass (`vk_dlaa_pass()`).

The jitter (`vk_dlaa_viewport()`):

- `jdepth`: the scene depth of the last frame DLAA ran on: passes drawing with it are jittered.
- `jw`, `jh`: its size: on the game's frame the camera's draws into any D32F depth of it are (`jitter_draw()`).
- `jx`, `jy`: the offset the next scene draws take, render pixels (window x right, y down).
- `ux`, `uy`: the offset this frame's scene draws took (DLSS and the motion vectors get these).
- `jit_ok`, `jit_used`, `hold`: jitter allowed this frame / a draw used it / frames left without it.
- `holds`, `jittered`: jitter holds; evaluations of a jittered scene.
- `jdraws`, `jfixed`: draws into a scene-size depth that took the jitter / that did not (full-screen).

The camera (`vk_dlaa_draw()`) and the motion vectors (`mv_pass()`):

- `cam_addr`: this frame's scene camera block (`vk_dlaa_draw()`), 0: none seen.
- `cam_size`, `cam_vp`: its size and the offset of its view-projection rows.
- `views`: this frame's scene contexts of light-target draws, each with:
- `seq`: `vk_exec_submit_seq` at the last draw that used it.
- `n`: draws.
- `got`: `cam` holds its rows 0-10 as the first draw read them (`vk_usnap_read()`: 1, 2), 0 not yet.
- `camb`: two `CAM_SLOT` slots: this frame's block and last frame's.
- `mvm`: `MVM_SLOTS` pairs of blocks for `runtime/shaders/dlaa_mv.comp`: the identity and the reprojection
  (`mv_pass()`).
- `cam_prev`: last frame's view-projection (rows 3-6 of its block), for the reprojection.
- `cam_slot`, `cam_prev_ok`: the slot this frame writes / the other slot holds last frame's camera.
- `mv_ok`: -1 not built, 0 failed, 1 ready.
- `masked`: frames with the characters' bias mask.
- `why`: history resets: scene size changed, no camera, scene target changed.
- `target_kept`: scene target changed and the history stayed (`target_changed()`).

Frames without a camera and what `mv_pass()` did about them (`dlaa_cam_keep`, `cam_note()`):

- `cam_none_draw`, `cam_none_snap`: no scene-context draw this frame / one, not in the evaluating submission's copy.
- `cam_at_draw`: camera taken from the bytes the draw read, where the evaluating submission lacks them.
- `cam_kept`, `cam_kept_max`: no camera, history kept with zero motion / runs cut short at `KEEP_MAX`.
- `cut_pos`, `cut_fwd`: last frame's camera position and direction (`cam_cut()`).
- `cut_ns`, `cuts`: when it was read; camera cuts that started a new history.
- `keep_run`: frames in a row kept without a camera. \nThe present, the settings and frame generation:\n\n-
  `seq_present`: `vk_exec_submit_seq` at the last present (`vk_dlaa_frame()`).
- `seconds`: present intervals with a second scene after DLSS ran.
- `on`: the dlaa setting, read at each present (the menu switches it live).
- `fgo`: frame generation runs and no upscaler does: inputs alone (`fg_only_frame()`), read at each present.
- `preset`, `preset_dirty`: the DLSS model features are created with (`preset_value()`); a change pending.
- `ids`: this frame's G-buffer target 0 (object classes in green), valid with `ids_ok`.
- `cfg_seen`: `cfg_version` when preset was read.
- `last_eval_ns`: `plat_now_ns` of the previous evaluation (FrameTimeDeltaInMsec).
- `fg_depth`, `fg_mv`: the last evaluation's depth and motion vectors, for frame generation.
- `fg_cam`: this frame's camera (`mv_pass()`), `fg_cam_ok` when known.
- `fg_fresh`: not handed out yet (`vk_dlaa_fg_inputs()`).
- `scene_last`: the last frame that ended drew a scene (`vk_dlaa_fg_scene()`).
- `presents`: `vk_dlaa_frame()` calls so far.
- `fg_at`, `hl_at`: presents when the inputs and the HUD-less picture were made (`fg_current()`).
- `hl`, `hl_srgb`: the picture without the HUD for frame generation (`vk_dlaa_fg_hudless()`): DLSS's output encoded to
  sRGB as the game's last pass encodes its picture, then copied bit for bit into an RGBA8 UNORM image, the format the
  present reads; with HDR on, DLSS's output as RGBA16F, as the game's float picture the present encodes (`hl_srgb`
  unused).

Before the bloom (`NX2SARIA_DLSS_PREBLOOM`, `prebloom_run()`):

- `pre_on`: the setting, read at each present.
- `pre`: this frame: the upscaler ran before the bloom chain; 1 back into the light target (no upscaling), 2 to the
  picture's size (`PB`), whose passes the retarget moves there.
- `pre_at`: where the last evaluation ran: 1 before the bloom, 0 at the game's upscaler.
- `comp`: this frame: the bloom composite was drawn into `PB.comp` (`vk_dlaa_retarget()`).
- `comp_game`: the game's target that composite was meant for.
- `pic_w`, `pic_h`: the size of the game's upscaler's output, the last one seen (`vk_dlaa_upscaler()`).
- `pre_misses`: frames with the setting on that found no bloom chain to run before.

`F`: what this frame drew into a D32F depth at least `DLAA_SCENE_MIN_W` wide before DLSS ran (`vk_dlaa_draw()`), for the
line a frame without a camera logs (`cam_note()`); cleared at each present (`vk_dlaa_frame()`).

- `zdraws`, `zother`: draws into the scene depth / into another such depth than last frame's (`jdepth`).
- `ocam`, `ocam_off`: of the scene-depth draws: with the scene context at c3 / of those, not into a scene.
- `ocam_off_fmt`: the format of the last such colour target.
- `c3_other`, `c3_size`: with another block at c3 (the size of the last such; 0: none bound).
- `seq0`: `vk_exec_submit_seq` at the frame's first scene-depth draw.
- `one_n`, `one_w`, `one_h`: after arming: passes with one colour target and no depth (`vk_dlaa_pass()`), the last's.
- `one_fmt`: its format; size and format are for a frame without the DLAA point (`vk_dlaa_frame()`).
- `second`: after DLSS ran: passes shaped like the light target's (another scene).
`FG_INPUT_PRESENTS` (4): an evaluation is taken at its own present, or the one after when the present thread is behind.

### `fg_current()`

What an evaluation left for frame generation is for the present of its own frame. Frame generation takes it only while
its swapchain is in place, so with frame generation off the last evaluation's images stayed marked fresh for as long as
it was off. Among them is the game's own depth target, which the game replaces when the scene's size changes: switching
the upscaler off and on again with frame generation set (the menu's Upscaling row passes through its choices without
one) handed that destroyed image to frame generation's preparation, and AMD's driver ended the process. Images made more
than `FG_INPUT_PRESENTS` presents ago are therefore never handed out, and `vk_dlaa_frame()` drops them when the upscaler
goes off or on.

### `vk_dlaa_fg_scene()`

Whether the frame that ended last drew a scene: a light target armed, or draws into a scene-size D32F depth. A frame
without one (the title, a movie, a load, the file menu, a fade) has nothing for frame generation, and its log line
counts it apart from the gameplay frames (`runtime/vk/vk_fg_stats.h`). Read on the present thread a frame late at most.

### `vk_dlaa_ffx_camera()`

Metres in one unit of the game's view space, for FSR's distance heuristics: one. Measured in Hyrule Field, the camera
orbits Link about 4 units behind him (the nearest scene depth, 0.975, is 1 - 0.1 / 4) at about 3 units a second, and
travels about 4.7 units a second while he runs; in centimetres (the 0.01 FSR had) he would be 4 cm from the camera.

### `vk_dlaa_fg_inputs()`

`reset`: DLSS started a new history on this frame (`evaluate()`; `fg_only_frame()` by the same rules without an
upscaler). `jx`, `jy`: 0 without an upscaler, whose frames are not jittered. Handed out as 0 before, so frame generation
was never told of a camera cut, another scene size or another target, and interpolated across them: an area load
generated frames between two unrelated pictures.

### `vk_dlaa_active()`

The same for `runtime/vk/vk_exec_draw.c` `untranslated_dispatch_fallback()`: while it holds, DLSS or FSR 3.1 scales the
scene and the stand-in of the game's own upscaler does not make the picture the screen shows (`vk_dlaa_upscaler()`).

### `inputs_on()`

The frame is armed and ended (`vk_dlaa_draw()`, `vk_dlaa_pass()`, `vk_dlaa_upscaler()`): the upscaler runs, or frame
generation takes the scene's inputs without one (`D.fgo`, `fg_only_frame()`).

### `use_fsr()`

The upscaler this frame runs with, as `vk_dlaa_frame()` read it. The menu can set `NX2SARIA_TAA` to dlss between that
and `evaluate()`; read again there while NGX is not up (DLSS starts only after a restart), it sent `evaluate()` to
DLSS's path with NGX's functions not loaded, a call to address 0 (issue #325).

### `idle_free()`

Which features may stay alive when another is made (`NX2SARIA_DLSS_IDLE_FREE`; `feat_released` counts the releases since
the last creation). A feature made while one with a larger input size is alive leaves that larger one drawing
alternating bands of rows black or noise from then on, for as long as it is used; seen for the pause screen's feature
(the picture's size in and out), in two ways:

- A pause of 3,600 frames frees the gameplay feature (`stale_sizes()`) and the unpause makes it again: every later pause
  was striped, in each of 8 scripted runs of pause, unpause, pause in which it had been freed and in none of 12 in which
  it had not, whatever the texture, write-back, HDR and frame generation settings;
- A DLSS mode picked in the menu makes a feature of a third size at the unpause, with nothing freed: the next pause was
  striped (performance to ultra performance in play).

Giving the kept feature its own sizes again at each evaluation changed nothing. A feature made while a smaller one is
alive leaves the smaller one as it was (the pause screen's after the gameplay one, at every first pause). So when a
feature is made, the alive ones with a larger input are released first, and after a release since the last creation all
of them (`release_kept()`); their images stay and each is made again when its size is next evaluated. Scripted on one
build, each way with the rule and with 2: the long pause and the mode change were striped without it and clean with it.
The input size may not be what matters: in every striped case the feature made was of another model (performance's or
ultra performance's) than the pause screen's, and the one run in which balanced's feature, of the pause screen's model,
was made again beside it without the rule was clean. The rule covers both readings. The values of
`NX2SARIA_DLSS_IDLE_FREE`:\n\n- 1 (default): idle features are freed;\n- 0: idle features are never freed (about 0.5 GB
more video memory after the first pause);\n- 2: idle features are freed and nothing is released when a feature is made
(as before; shows the fault).

### `vk_upscaler_device_idle()`

The upscalers' waits for the device to be idle before they free what the GPU may still use (this file,
`runtime/vk/vk_ffx.cpp`, `runtime/vk/vk_ffx4.cpp`), with the queue lock held: the present's flip, the completion
thread's hold release and every upload submit wait as long. `NX2SARIA_TRACE=perf` names them "upscaler GPU waits" on a
slow frame's line, together with `vk_upscaler_gpu_wait()`'s timeline waits after a refused creation, which touch no
queue.

### `feat_release()`

A DLSS feature's release, timed as "upscaler releases" (`NX2SARIA_TRACE=perf`).

### What the upscalers let go of while the GPU may still use it

The lists of `runtime/vk/vk_up_defer.h` (`upd_put()`, `upd_take()`), here and in the FSR files.

A size's images and feature (a third scene size, a DLSS model change, a size unused for `SIZE_STALE` evaluations), the
features `release_kept()` and `stale_sizes()` release, a snapshot of the reactive mask, the HUD-less picture of another
size, prebloom's images, what `retire_idle()` gives back, and FSR 3.1's and FSR 4's contexts (`runtime/vk/vk_ffx.cpp`,
`runtime/vk/vk_ffx4.cpp` `context_for()`) were freed after a wait for the whole device to be idle, on the replaying
thread with the queue lock held: the present's flip, the completion thread's hold release and every upload submit waited
as long, at each pause-screen size, reload, window size, dynamic resolution step or mode change (16 to 45 ms in play
together with a DLSS feature's create). Nothing waits for them now:

- Images only the replay uses go to `runtime/vk/vk_memory.c`'s deferred destruction (`vk_defer_image()`,
  `vk_defer_view()`), stamped with the submission being recorded, which comes after every submission that can name them;
- A DLSS feature goes into `dead` with the fence of the last submission that created or evaluated it (`feat_fence`) and
  is released once the GPU is past it, which is what NGX asks of a feature's release; on this thread, which makes every
  NGX call (NGX is not thread safe);
- An image frame generation can read at a present (a size's motion vectors, the HUD-less picture) goes into `dead`
  behind a present gate (`FG_LATE_PRESENTS`), and from there to the deferred destruction;
- FSR contexts go into lists of their own in `runtime/vk/vk_ffx.cpp` and `runtime/vk/vk_ffx4.cpp` (outside Windows FSR
  3.1's in `runtime/vk/vk_fsr3.c`, through `vk_ffx_dead_take()`), taken at the same points.

The lists are taken at every evaluation (`evaluate()`), at every pass while none runs (`retire_idle()`), and where
something is let go of (`vk_upscaler_let_go()`). What still waits: a feature or context creation refused while ones let
go of are in flight waits for the GPU to pass those (`vk_upscaler_gpu_wait()`), and a full list for the device
(`vk_upscaler_settle()`). `NX2SARIA_UPSCALER_IDLE_WAIT=1` waits for the device wherever something is let go of, as up to
0.10.1, but does not bring 0.10.1 back exactly: what was let go of still goes through these lists and the deferred
destruction, an image a present reads keeps its present gate, what the submission being recorded names stays until it is
done, and a refused DLSS creation with features let go of still listed waits and tries again, where 0.10.1 turned DLAA
off.

`dead` is this file's list; only DLAA's hook thread touches it (`vk_dlaa_pass()` and what it runs), as the state of
`evaluate()`. Its kinds: `UPD_FEAT`, a DLSS feature; `UPD_IMG`, an image with its allocation (`aux`); `UPD_VIEW`, a
view.

`FG_LATE_PRESENTS` (8): an image of this file that frame generation can read at a present goes this many presents after
it was let go of. `present_image()` (`runtime/vk/vk_backend.c`) calls `vk_dlaa_frame()` before it waits for the present
thread to take the job before it, so 3 presents on, the present job that last read it has been submitted and flipped; 1
more for a present from another thread than the submitting one; and 4 more for frame generation that reads it during the
flip on queues of its own (`runtime/vk/vk_dlssg.c`: inputs valid until the present), which no timeline value of the
runtime covers. The deferred destruction then stamps it with the submission being recorded, which comes after those
presents.

### `presents_now()`

`D.presents` from the hook thread (`vk_dlaa_frame()` counts it on the presenting thread).

### `vk_upscaler_idle_wait()`

`NX2SARIA_UPSCALER_IDLE_WAIT`, read once: 1 waits for the device wherever an upscaler lets go of something.

### `dead_release()`

One entry of `dead`, as `upd_take()` hands it out, timed as "upscaler releases". At teardown (`vk_dlaa_release()`: the
device is idle and about to go): images and views are destroyed at once.

### `upscalers_drain()`

What the upscalers let go of goes once the GPU is past it: DLSS's here, FSR's in `runtime/vk/vk_ffx.cpp`,
`runtime/vk/vk_ffx4.cpp` and `runtime/vk/vk_fsr3.c`.

`vk_ffx_dead_take()` returns at once with both of its lists empty.

### `vk_upscaler_settle()`

A wait for the device, then everything the upscalers let go of that a submission made so far names goes. An image a
present can read keeps its present gate: the device's idle does not cover a present still to come.

The count of submissions issued is read before the wait: every submission up to it is done after it.

### `vk_upscaler_gpu_wait()`

- The value waited for is at most the submissions issued: a submission still being recorded cannot finish while this
  thread records it.
- The queue lock is taken and released first: the replay's held submissions go to the queue (`runtime/gfx/gfx_present.c`
  `vk_queue_lock()`).
- A wait on the timeline from the host touches no queue, so no queue's lock is needed (unlike the device's idle).

### `dead_put()`

The list full, which the GPU's progress and the presents do not let happen: room after a wait for the device, and if a
present gate still holds every entry, they all go as before this list existed, the device idle.

### `retire_size()`

A size let go of while the GPU may still use it: the images only the replay uses to the deferred destruction, the motion
vectors (frame generation's, `D.fg_mv`) behind the present gate, the feature once the GPU is past its last create or
evaluation. The entry is free at once.

A view is put before its image: `upd_take()` and `runtime/vk/vk_memory.c` keep the order.

### `retire_feat()`

A size's feature alone (`release_kept()`, `stale_sizes()`), released once the GPU is past its last create or evaluation.
`count`: it counts as a release for `release_kept()`'s next call (`feat_released`).

### `destroy_size()`

The images and views of a size, then its feature. The device must be idle, or the GPU past every use of them:
`vk_dlaa_release()` (teardown), `idle_release()` (unused for `IDLE_FREE_PRESENTS` presents). Whatever was not made is
skipped, so a half-made size goes the same way.

### `free_size()`

A third scene size takes the least recently used one's entry (`size_for()`): its images may still be in flight.

### `size_for()`

The entry for a scene and output size: its output (RGBA16F at the output size, written by DLSS, then blitted into the
target) and motion vectors (RG16F at the scene size, render pixels), both kept in GENERAL like every image of the back
end. The game's dynamic resolution switches between a few sizes; each keeps its images, and its own feature unless it
runs on the DLSS mode's (`range_host()`); the least recently used goes for a new one.

### Timing

`NX2SARIA_DLAA_TIMES=1`: GPU timestamps around the camera motion dispatch, the per-object motion pass and DLSS's
evaluation, and the CPU time of NGX's call and of draining the recorder before it, summed and logged every
`TIMES_SUMMARY` frames:

```
dlaa times: gpu camera motion a ms, object motion b ms, DLSS c ms (frames timed); cpu: recorder drain d ms, NGX evaluate
e ms, motion pass recording f ms
```

Each frame takes its own slot of `TIMES_SLOTS`; a slot's results are read when it comes round again (by then the GPU is
long done with it; not ready: that frame is left out).

The state, `T`:

- `on`: -1 not read.
- `used`: the slot holds a frame's timestamps (all `T_N` written).
- `gpu`: ms summed: camera motion, object motion, DLSS.
- `cpu_drain`, `cpu_ngx`, `cpu_mv`: ns summed.

### `times_begin()`

At the start of a frame's timing (`mv_pass()`): the slot's last results are read and it is reset.

The device's `timestampPeriod` takes ticks to ms.

### Motion vectors

The camera's motion first (`runtime/shaders/dlaa_mv.comp`): this frame's scene camera block is copied out of the
replay's uniform snapshot (`runtime/vk/vk_usnap.c`, as of the submit call) into one slot of `camb`, last frame's stays
in the other, and the compute pass reprojects every pixel's depth through the two (`inv4()` and `mv_pass()`). Then the
per-object pass (`runtime/vk/vk_exec_draw.c` `vk_mvobj_execute()`) writes the motion of whatever moves on its own over
it.

- `CAM_BYTES` (256): vec4 rows 0-15: view, view-projection (3-6), projection, inverse view.
- `CAM_BLOCK` (0x940): the whole block, kept for the per-object motion pass's vertex shaders
  (`runtime/vk/vk_exec_draw.c`).
- `CAM_SLOT` (0xa00): the stride of a `camb` slot.
- `MVM_SLOTS` (16): frames the GPU may still be reading the reprojection blocks of.

`MvPush`: `runtime/shaders/dlaa_mv.comp`'s push constants in its layout (std430: `dz` at offset 32); `dz` is the
projection's [2][2], [2][3], [3][2], [3][3].

### `inv4()`

The reprojection from this frame's clip space to last frame's, last \* inverse(this), in double precision: the shader
took each pixel back to world space through a float inverse of the view-projection, and with the game's world
coordinates in the thousands and its far plane the result was noise of a fraction of a pixel and more on distant
surfaces, which DLSS followed into a shimmer like heat haze (roofs and trees, most of all in the upscaling modes). The
shader is unchanged: its "this frame" block gets the identity and its "last frame" block this matrix, so it applies just
the one matrix to each pixel. A still camera gives the identity, and zero motion exactly.

### `ocam_vp()`

The game's camera from its scene context block (`vk_dlaa_draw()`): its scene vertex shaders take the world position to
the camera through rows 0-2 (a 3x4 view) and then to clip space through rows 7-10 (the projection, 4x4), so the
view-projection is rows 7-10 times rows 0-2.

### `fg_store()`

Frame generation's camera (`vk_dlaa_fg_camera()`) from the game's block: the projection is rows 7-10, the view rows 0-2
(rows: right, up, back; the camera looks down -z), m the reprojection from this frame's clip space to last frame's. Each
matrix is handed out transposed, for row vectors.

### `plane_dist()`

The view distance in front of the camera at which the projection P (row-major, column vectors, looking down -z) gives
clip depth `z_ndc` = k: `(P[10] z + P[11]) / (P[14] z + P[15]) = k`, solved for z. INFINITY where no distance gives it.

### `proj_planes()`

What the depth buffer's 0 and 1 stand for, from the frame's projection and the device's depth mode. With NVN's
`NEAR_IS_MINUS_W` (clip depth -w..w, which the pipelines keep through `negativeOneToOne`, `runtime/vk/vk_pipeline.c`) a
fragment stores `z_ndc` \* 0.5 + 0.5, so 0 is `z_ndc` = -1; with `NEAR_IS_ZERO` it stores `z_ndc`. Both are 0..1 in the
D32F target (the viewport's depth range, `runtime/vk/vk_exec_draw.c`), and the -w..w mapping stores exactly what a 0..w
projection with the same planes would: `(f / (f - n)) (1 - n / distance)`, the convention DLSS, Streamline and FSR take.
The game's planes are not fixed: in Hyrule Field its z row is (-1.00001, -0.200001), near 0.1 and far about 25,000 (the
view's units are metres), and the camera before it had far about 1,000. Taken as 0.1 and 1e8 (no far plane), a distance
d reads as `(f - n) d / (f - d)`: 1,000 as 1,042, 10,000 as 16,650, and the far plane's neighbourhood without bound. So
the planes come from the projection every frame; a projection whose stored 0 is not in front of the camera keeps the
last frame's planes.

- The distances stored as 0 and 1 are taken (`plane_dist()`); when the first is not the smaller the depth is reversed:
  the near plane is stored as 1.
- One line at the start and whenever the planes change (the first 20 times).

### `fg_camera()`

- The position is `-R^T t`, R the view's 3x3 and t its last column.
- Until a projection gives the planes: near 0.1 and no far plane.
- `NX2SARIA_MOUSE_TRACE=1` (test runs, `runtime/input/mouse_direct.c`): the camera's direction each frame.

### Frames without a camera

`dlaa_cam_keep` (`NX2SARIA_DLAA_CAM_KEEP`; 1 by default, 0 restores the old handling). Measured in Kokiri Forest during
dialogue (Saria, dlss = performance): about 1 frame in 20 had no camera, and each started a new history twice, on that
frame and on the next (nothing to reproject from): 24 to 26 resets per 300 frames. Those were frames the event order
held past their present (`runtime/vk/vk_exec.c`), which then ran in a submission without a uniform copy (the present
thread's empty one), so the lookup at DLSS found nothing. With the change: no camera reset there in 3600 frames but the
first. Two things change:

- The game's camera rows are taken from the bytes the scene draws themselves read (`vk_dlaa_draw()`, `views[].cam`), not
  looked up again in the uniform copy of the submission DLSS runs in, which on some frames has none or lost the block to
  a GPU write.
- A frame that has no camera at all keeps DLSS's history: its vectors are zero (the camera taken as still) and last
  frame's camera stays the one the next frame reprojects from. At most `KEEP_MAX` frames in a row: a longer stretch is
  something else (a cut scene with a camera of its own), and a still history there would trail what moves.

### `cam_note()`

One line for each of the first frames without a camera: what the frame drew into the scene depth, which submission the
scene context was seen in and why the evaluating one lacks it, and what was done.

### Camera cuts

`NX2SARIA_DLAA_CUT` (default 1; 0: off). Only a lost camera (`mv_pass()`), another scene size or another target started
a new history, so DLSS and frame generation (`vk_dlaa_fg_inputs()`) carried their history across a camera that jumped:
an area load or a spawn moves it 5 to 180 m in one frame. Such a frame now starts a new history, as NVIDIA's DLSS guide
asks for a camera cut: a move over 5 m, or a turn over 10 degrees together with a move over 0.5 m, both per 60 Hz frame
and scaled by the time since the last camera. Camera traces of play in the Fire Temple (`NX2SARIA_MOUSE_TRACE`) had a
median of 0.05 m and 0.08 degrees a frame. A turn alone (the mouse camera, which orbits about 3 m from Link) does not
count, and the turn with a move counts only from `CUT_TURN` degrees: a quick swing of the orbiting camera turns 11 to 18
degrees and moves 0.9 to 1.4 m a frame, and taken for a cut at 10 degrees it started a new history at every frame of the
swing (nine in a row in Hyrule Field), which at FSR's or DLSS's smaller modes showed the scene at its render size for
those frames, blocky and flickering. Its motion vectors carry it as well as any other turn. Each cut is logged, up to
24.

### `cam_cut()`

`v`: rows 0-2 of the game's scene context, the view (`fg_camera()`).

The thresholds are scaled by the 60 Hz frames since the last camera.

### `mv_pass()`

The pass, recorded through the recorder like the replay's own commands. 0: no camera this frame (the vectors stay zero
and DLSS starts a new history); 2: no camera, but the history is kept (the vectors stay zero, `dlaa_cam_keep`). `bias` >
0 with this frame's G-buffer classes (`D.ids`) also writes `e->mask`, the bias mask (`runtime/shaders/dlaa_mv.comp`);
`*masked` says whether it did.

- `view` is the `views[]` entry of the camera.
- This frame's view-projection as the scene draws read it (the game, `dlaa_cam_keep`) or as the snapshot holds it, for
  the reprojection; kept as last frame's for the next.
- A camera block of `CAM_BLOCK` bytes is kept whole, for the per-object motion pass (the deferred engine's only).
- Without last frame's camera there is nothing to reproject into: zero motion, new history.
- The shader reads rows 3-6 of each of the two blocks.
- The class mask needs the G-buffer at the scene size; otherwise bindings 4-7 get the mask images as stand-ins (the
  shader returns before reading them with bias 0).
- The depth check (`runtime/shaders/dlaa_mv.comp`, `depth_prev`) where nothing else gives characters their own motion:
  the game's frame, which has neither the per-object pass nor the class buffer. Its distances take the projection's
  depth terms from rows 7-10 of the scene context (`ocam_vp()`). `NX2SARIA_DLAA_ZFIT=0` (test): the camera's motion
  alone, as before.
- `NX2SARIA_MV_CHECK` (test) gets the motion image here (`runtime/vk/vk_mvcheck.c` `vk_mvcheck_camera()`).
- The characters' own motion over the camera's (`runtime/vk/vk_exec_draw.c` `vk_mvobj_execute()`).
- Not on a frame that starts a new history, nor on the first frame of another render size (`D.stepped`, `evaluate()`):
  that frame was left out before the history could stay across a size, and it still is.
- With timing on, the DLSS marks follow in `evaluate()`.

### `vk_dlaa_prev_camera()`

Last frame's camera block for the per-object motion pass, recorded before this frame's `mv_pass()`: that pass writes
this frame's into slot `cam_slot` and leaves the other slot, last frame's, as it is. 0: none (no history).

The per-object pass knows the deferred engine's block only: another size gives 0.

### Reactive mask

Particles, smoke, sparks and other transparents have no motion vectors of their own: the motion image holds the
surface's behind them, and DLSS carried their old colour along that surface (trails behind sparks, smoke smeared with
the camera). DLSS takes a mask of where to trust this frame's colour over the history (NGX
"DLSS.Input.Bias.Current.Color.Mask"). It is made from the picture itself, at no cost on the CPU: the light target is
copied when the transparents start (the pass that arms DLAA, `vk_dlaa_pass()`) and compared with what DLSS gets
(`runtime/dlaa_mask_spv.h`, `tools/make_dlaa_mask.py`): the share of a pixel's colour that changed, from `MASK_THR` on,
times `MASK_GAIN`. Off by default (`NX2SARIA_DLSS_REACTIVE=1` turns it on, `NX2SARIA_DLAA_MASKDEBUG=1` shows the mask
instead of the picture): the pass that arms also draws the sky and blended fog layers over the solid scene, and the mask
covered the sky, sunlit trees and roofs and a wide fog region besides the particles (seen with `NX2SARIA_DLAA_MASKDEBUG`
at 19:45 in the first save), where DLSS would give up its anti-aliasing. It needs the particle draws alone (drawn again
into the mask, like the motion draws) before it can be on. In the upscaling modes the light target DLSS reads also holds
the bloom by then; the copy would have to be compared at the end of the scene instead.

The state, `R`:

- `Snap`: per light target size and format: its copy at arming, and the mask.
- `cur`: this frame's copy, NULL: none.
- `ok`: -1 not built, 0 failed, 1 ready; `knob`: -1 not read.
- `made`, `missed`: frames with a mask; frames DLSS ran without the copy.

### `destroy_snap()`

A snapshot's images and views. The device must be idle (`vk_dlaa_release()`), or the GPU never used them (`snap_for()`'s
half-made pair).

### `retire_snap()`

The same for a snapshot the GPU may still use (only the replay reads it): to the deferred destruction.

### `free_snap()`

A fourth size: the evicted snapshot's images may still be in flight, so it is retired.

### `snap_for()`

What was made of a pair that could not be finished is destroyed at once: the GPU never used it (an evicted one's images
went before).

### `mask_snapshot()`

At arming: the light target as it is before the transparents, outside any pass.

### `mask_pass()`

Before DLSS: the mask from the copy and the light target (view `cv`). NULL: none this frame.

### `hl_destroy()`

The HUD-less picture's images, a half-made pair too. The device must be idle, or the images not yet used.

### `retire_hl()`

The same for a HUD-less picture the GPU may still use: `D.hl`, which a present copies for frame generation
(`runtime/gfx/gfx_present.c` `hudless_record()`), behind the present gate; the sRGB copy, which only the replay uses, to
the deferred destruction.

### The free camera's view of the ground

`runtime/diag/debug_tools.c` `dbg_pad()`, ground avoidance: while the free camera flies with that on, nine of the view
distances `mv_pass()` wrote (a grid over the lower part of the view, where the ground is) go back to the host, which
works out how far below the camera the ground is (`ground_under()` there). A slot is read `FC_SLOTS` evaluations after
its copy, when the GPU has long been past it.

### `fcam_probe()`

- It needs `dist_ok`: `mv_pass()` wrote this frame's distances (the game's frame).
- The replay's viewports have negative heights: row 0 is the top of the view.

### `vk_dlaa_release()`

The device is idle here. It releases the NGX features and parameters before their Vulkan resources, then shuts NGX down
even if initialization succeeded but the layout check failed. The Linux core's Shutdown1 takes an extra output pointer
compared with the SDK's public wrapper; it reports the number of devices still using NGX.

- First what was let go of: the features before NGX goes, the images at once.
- Frame generation's feature and parameters (`vk_ngxfg_evaluate()`) go before NGX does.
- The reprojection blocks' buffer is mapped for good: the destroy unmaps it.
- Without `N.device`, NGX never came up on this device (DLAA off, FSR 3.1, or its init failed), and there is nothing to
  shut down.
- The shutdown runs with the queue lock held: NGX may wait for the device itself.

### Jitter

The scene's draws (every pass drawing with the scene depth, until the DLAA point) have their viewport moved by a
sub-pixel Halton(2,3) offset, which shifts what they rasterise as a jittered projection would; full-screen passes that
shade by window position are unaffected. Jitter only follows a frame DLSS ran on: a jittered frame that DLSS does not
resolve shakes on screen, so a missed DLAA point stops the jitter for a second.

### `jitter_draw()`

Whether a draw takes the jitter, before DLSS ran this frame. The deferred frame: every draw into the scene depth. The
game's frame: every draw that projects through the camera (it binds the scene context at c3, `vk_dlaa_draw()`) into a
D32F depth of the scene's size, whichever image that is. The game draws its depth pre-pass into the scene depth, copies
that depth into a second image of the same size, and then shades the same geometry into the light target against the
copy with the depth test EQUAL. Jittering only the draws into the first image left the shading draws unjittered against
jittered depth: EQUAL failed on nearly every pixel, the light target kept its clear colour, and the ground and
characters went white while the sky stayed right. Its full-screen passes drawn with a scene depth attached (the bloom
composite DLSS reads) do not take the jitter either: they read the jittered scene through their own texture coordinates,
and moving them too would shift the picture DLSS gets by a second offset.

### `vk_dlaa_viewport()`

- A pass moved to the picture's size (`vk_dlaa_retarget()`); without a game viewport it covers the pass already.

### `vk_dlaa_jitter_check()`

It also counts the draws into a scene-size depth, with the jitter or without (`D.jdraws`, `D.jfixed`).

### `vk_dlaa_draw()`

Takes a lock: the replay workers' scene draws count at once (the chunks of `runtime/vk/vk_exec.c`).

### `dlaa_draw_locked()`

- `NX2SARIA_DLAA_SURVEY=1` (test): what is drawn into the armed light target: the scene draws writing depth, by depth
  test (reversed depth: GREATER/GEQUAL), and the draws into the light target by blend, depth write and depth test.
- The scene camera of a draw into the scene depth: the deferred engine binds a 0x940-byte block at c4 with the
  view-projection in rows 3-6 (0x30); the game's opaque scene draws bind a 0x3500-byte scene context at c3, whose rows
  0-2 and 7-10 their vertex shaders use as the view and the projection (`ocam_vp()`). Its full-screen passes bind a
  0x200-byte block at c3 instead, which is not the camera. Without a camera there is no camera motion and no jitter
  (`next_jitter()`), so DLSS runs on each frame alone without shaking the picture.
- The deferred engine's block is at c4, index 1: the replay keeps user banks from c3 on.
- On the game's frame the camera is the scene context most light-target draws use this frame (`D.views`).
- The camera rows as this draw reads them, from its own submission's copy: DLSS runs in a later submission on some
  frames, whose copy may not hold the block (`mv_pass()`, `dlaa_cam_keep`).

### `jitter_on()`

`NX2SARIA_DLAA_JITTER=0` (test): no jitter. Read once.

### `next_jitter()`

The jitter moves on right after DLAA ran, not at the present: the replay can record the next frame's scene before the
present of this one is handled, and a step taken there landed in the middle of a frame (worse after an off/on switch,
which shifted that order). Jitter needs a frame DLSS ran on; a frame without the DLAA point stops it. The sequence is as
long as NVIDIA's DLSS guide asks for the upscaling ratio, 8 x (output / scene)^2 phases (18 in Quality, 24 in Balanced,
32 in Performance, 72 in Ultra Performance), and 16 at least, the length DLAA has used from the start.

- Jitter only where the camera is known (`vk_dlaa_draw()`): the deferred engine's block or the game's scene context. On
  the game's frame the draws that take it are the camera's into every scene-size depth (`jitter_draw()`); taking only
  the draws into DLSS's own depth image turned the picture white, and jitter was off there until that was found.

### `preset_value()`

The DLSS model (`NX2SARIA_DLSS_PRESET`, live): default takes NVIDIA's model per mode (`evaluate()`: K for DLAA, Quality
and Balanced, M for Performance, L for Ultra Performance), 0 here. A letter a..m takes that model in every mode: k pins
K, j is the other transformer model, which NVIDIA describes as slightly less ghosting at the cost of extra flicker, e
the older CNN model, cheaper on slower RTX cards. The value is an `NVSDK_NGX_DLSS_Hint_Render_Preset` (1 A ... 13 M),
read when a feature is created; a change recreates the features (`preset_check()`).

### `char_bias()`

How strongly DLSS favours the current frame on characters and where they just were (`NX2SARIA_DLSS_CHAR_MASK`, percent,
live; 0 off). Where a character's vectors are the camera's only (`dlaa_objmv` = 0, or a draw the per-object pass skips),
one that moves on its own drags a faint copy of itself through DLSS's history; the bias mask makes DLSS take less of the
history there, at the cost of some of its anti-aliasing on those pixels. The game's frame has no class buffer to find
characters by: there the mask covers the pixels whose depth the camera's motion does not explain
(`runtime/shaders/dlaa_mv.comp`, the depth check).

### `perf_quality()`

The quality setting DLSS is created with, from the scene's share of the output width.

### `window_line()`

Every 300 frames DLSS ran, the history resets and camera gaps of those frames, while there were any (at most 400 lines a
run): the measure for `dlaa_cam_keep` at one spot of the game.

### `target_changed()`

Another light target than last frame's. The game draws its scene into one of a few pooled targets of the same size and
moves to another whenever its pass list changes, in the middle of play with the camera and the scene carrying on: 35
times in a walk of a minute through Kokiri Forest, three of them within eight frames on the ladder below the tree house.

The history is the upscaler's own and not in the target, so where this frame's motion vectors are valid (`cont`: the
camera of this frame and of the last one, `mv_pass()` 1) the history stays. A new history at each change showed as a few
soft, grainy frames every time, a flicker over the whole picture. Without valid vectors, and with
`NX2SARIA_DLAA_TARGET_KEEP=0` (test), a new history as before. The first few are logged with the new target's size, to
tell a target the game made anew from the ones it alternates between.

### `query_vram()`, `vk_dlaa_vram()`

DLSS's own video memory, the features and their history (NGX's DLSS statistics callback with the capability parameters),
for the log's VRAM breakdown; -1 until known (`query_vram()` asks). Asked on the replay thread, which makes every other
NGX call, every 600 evaluations.

### `stale_sizes()`

Frees the sizes other than `cur` that no evaluation used for `SIZE_STALE` evaluations: the GPU finished with them long
ago (the CPU is never that many presents ahead), so their images and feature go at once (`retire_size()`, the drain in
`vk_upscaler_let_go()`). `host` is the size whose feature `cur` ran on (`cur` itself, or the DLSS mode's:
`range_host()`), which stays as well. A size that is in use on another's feature can still hold one of its own from
before that other was made: unused as long, it goes alone.

### `release_kept()`

Before a feature is made: the features of the other sizes with a larger input go, or all of them (`all`: a feature was
released since the last creation, `feat_released`); their images stay. The GPU may still be running an evaluation of
theirs: those whose last submission it has passed are released here, before the creation, and the others once it has
(`retire_feat()`, `vk_upscaler_let_go()`), after it. None of them is evaluated again: its size makes a new one.

- `mine`: this call's features, to tell them in `dead` from those let go of before.
- This call's features still in `dead`: the GPU is not past their last frame (a feature goes into `dead` once and leaves
  it only through its release, and no feature is made in between, so a handle there is one of them).

### `edge_fill_mode()`

The outer columns and rows of what a jittered pass draws into. The jitter moves the scene's viewport by under half a
pixel (`vk_dlaa_viewport()`), which keeps every pixel centre inside it, yet on an RX 7900 XT (AMD's Windows driver) the
draws of a viewport moved left left the last column out, and of one moved up the last row: the driver evidently limits
drawing to the whole pixels inside the viewport. Those pixels keep what was there before the pass: in a frame dump
(`NX2SARIA_DUMP`, Hyrule Field) the depth's last row kept its clear value through every scene pass and the light target
the sky's colour, and in a village the light target's clear colour, white. With `NX2SARIA_DLAA_JITTER=0`, and without
DLSS or FSR, every row is drawn.

So when a pass that took the jitter ends (`runtime/vk/vk_exec.c` `vk_exec_pass_end()`), the outermost row and column on
each side of its colour targets and its depth are filled from the ones next to them, before anything reads them. Filling
only the armed pair right before the upscaler read it (`evaluate()`, which still does it for a target a dispatch wrote)
was too late for whatever reads the scene in between: water reads the scene behind it and showed bright patches along
the bottom in a fifth of the shots of a run through a stream (none with the jitter off), and the bloom and the
anti-aliasing pass read the row too. (The white line in a player's clips had a second cause, the upscaler's picture
never reaching the screen with the HUD hidden: `vk_dlaa_upscaler()`.) A copied edge is one render pixel at the picture's
border, which the upscaler blends with its history.\n\n`NX2SARIA_DLAA_EDGE_FILL`: 1 (default) at each jittered pass's
end and before the upscaler; 2 only before the upscaler (as 0.9.1 did, for comparison); 0 not at all.

### `edge_copy()`

One image's border from the texels next to it, in one copy: the columns and rows without their ends, then the four
corners from their diagonal neighbours. Every source is inside the border and every destination on it, so no region
reads what another writes. The image is in GENERAL and nothing is still drawing into it.

### `vk_dlaa_pass_end()`

A barrier first: the pass's draws are done, and its targets are in GENERAL.

### `edge_fill()`

Before the upscaler reads the armed pair (`evaluate()`): once more, for a light target a dispatch wrote after its last
pass. The pair is at level 0 of the scene's size.

### FSR 3.1's second sharpening pass

FSR 3.1 in its scaling modes is soft where FSR 4 is not, most of all while the camera moves. Measured on a village save
while the camera turned at a steady rate (present screenshots every 0.4 s, the mean absolute Laplacian of the scene's
luminance as the fine detail): FSR 3.1 Native AA 6.7, FSR 4 Performance 5.9 and Ultra Performance 5.4, FSR 3.1
Performance 4.0 and Ultra Performance 3.3 with its own sharpening at the slider's default of 80, 5.0 and 4.1 with it at
100. FSR 3.1's tuning constants (velocity factor, accumulation per frame, shading change scale) gave nothing or took
detail away. So below a render scale of three quarters FSR 3.1's own sharpening goes towards its maximum
(`runtime/vk/vk_ffx.cpp` `vk_ffx_upscale()`) and its picture gets a second pass of RCAS here
(`runtime/shaders/dlaa_sharpen.comp`), before the HUD is drawn over it: both grow as the scale falls, full from
Performance down, and both follow the sharpness slider, 0 turning them off. `NX2SARIA_FSR3_SHARPEN=0` (test): no second
pass.

### `sharpen_init()`

It needs `mv_init()`: that one's sampler serves the texel fetches.

### `vk_dlaa_fsr3_boost()`

How much of the extra sharpening a render scale takes: none from three quarters of the output's width up, all of it from
half (Performance) down. `runtime/vk/vk_ffx.cpp` raises FSR 3.1's own sharpness by the same share.

### `fsr3_sharpen()`

FSR 3.1's picture `e->out` (w x h rendered, ow x oh out) sharpened once more, in place: RCAS into `e->sharp` and back.

RCAS takes its sharpness in stops below the most (0). At full strength (0.4 stops at the slider's 80, as
`runtime/vk/vk_fsr.c` maps it for the present) the pass took Performance's fine detail to 8.2, past Native AA's 6.7:
grain on every surface. It runs `SHARPEN_STOPS` (1.5) weaker, and weaker still above Performance's scale.

### One feature for a range of render sizes

`NX2SARIA_DLSS_RANGE` (`dlss_range` in `nx2navi.ini`; 1 on; 0, the default until it has run on a card: every render size
has a feature of its own).

The render size changes under one output size when dynamic resolution steps it (`runtime/pacing/dynres.c`) and when a
screen renders at another share than play. With a feature per size, each new size cost a feature creation (tens of
milliseconds and a wait for the GPU, `release_kept()`) and each change a new history, a frame or more of soft picture.
DLSS takes more than one render size on a feature. NGX's optimal settings callback (the capability parameters'
"DLSSOptimalSettingsCallback") answers, for an output size and a quality value, with the smallest and the largest render
size a feature of that mode takes ("DLSS.Get.Dynamic.Min.Render.Width" and its three siblings), both included; a mode or
a DLSS library without dynamic resolution answers with one size for both. An evaluation names its render size as the
sub-rectangle of its inputs ("DLSS.Render.Subrect.Dimensions.Width" and ".Height", which `evaluate()` sets on every
frame, and where it starts in each input, "DLSS.Input.Color.Subrect.Base.X" and the like); a size outside the range
fails the evaluation. Feature creation has no flag for this (`NVSDK_NGX_DLSS_Feature_Flags` holds none). NVIDIA's DLSS
guide adds two things: a feature keeps the model of the size it was created with, and it is to be created for the
largest render size it will see, because an input larger than the one it was created for makes DLSS allocate its buffers
again in the middle of play.

So the feature of the DLSS mode the player set, the one created at that mode's own render size, also runs every smaller
render size from DLSS's minimum up (`range_host()`): dynamic resolution steps down from the player's mode and never
above it. Such a frame is evaluated on that feature with its own images, all of them exactly its render size
(`size_for()`), as the sub-rectangle at their top left, and the history stays (`evaluate()`). A size above the feature's
own, below DLSS's minimum, of another shape, with another output size, or met while the player's mode is another one,
has a feature of its own; so has every size where DLSS reports no range.

A run in which the render size never changes creates its feature with the parameters it had without this, and evaluates
it with the same ones; the query after the creation is the only call added.

Two things still treat the first frame of another render size as a new start, although DLSS's history stays: the
per-object motion pass leaves it out (`mv_pass()`), and frame generation is told to start anew (`D.fg_reset`), since the
depth and the motion vectors it gets are of another size than the frame before's.

`range_refused`: DLSS failed an evaluation inside the range it reported (`evaluate()`): off for this run.
`runtime/patches/widescreen.c` `wide_dlss_share()` gives the DLSS mode's share of the picture (1 DLAA), 0 off.

### `setting_quality()`

The quality value of the DLSS mode the player set (`NX2SARIA_DLSS`): the one `perf_quality()` gives a scene at that
mode's share of the output, so the feature created at the mode's own render size carries it.

### `range_query()`

DLSS's answer for an output size and a quality value: the render size it would pick itself (`opt`) and the smallest and
the largest a feature of that mode takes (`lo`, `hi`). 0: the DLSS library has no such callback, or it failed. Asked on
the replay thread with the capability parameters, like the statistics (`query_vram()`).

- "RTXValue" is set to 0: older DLSS libraries expect it set.
- A library without dynamic resolution leaves these four unset: the size it picked is then all of the range.

### `range_note()`

After s's feature is made: whether it also runs smaller render sizes, from which size up, and one line saying so. It
does when it is the feature of the player's DLSS mode and its own size lies inside the range DLSS reports, above the
range's smallest size.

### `range_host()`

The size whose feature a frame of e's size runs on: the one of the player's DLSS mode when e's render size is inside
what that feature takes, no larger than the size it was created for on either axis, no smaller than DLSS's minimum, and
of its shape within 3 % (sizes round to multiples of 8; DLSS wants the render size's shape to be the output's); e itself
otherwise. That feature may have been released since (`release_kept()`): `evaluate()` then makes it again, for its own
size. Of two that fit, the one the frame before ran on, whose history then carries on.

A size that is itself ranged with the mode's quality value is the mode's own size, and its own host.

### `evaluate()`

The evaluation of the upscaler on the armed scene, on the thread that replays the pass. In the order of the function:

`size_changed` is set here when the scene the upscaler runs on has another size than the last one;
`vk_dlaa_resized_take()` reads it.

- Once a frame while the upscaler or frame generation's inputs run, what the GPU is past goes (`upscalers_drain()`).
- Without an upscaler (`D.fgo`): frame generation's inputs alone, at the same point of the frame (`fg_only_frame()`).
- `NX2SARIA_TAIL_EARLY`: from here on the frame's tail, in a command buffer of its own (`runtime/vk/vk_exec.c`
  `vk_exec_tail_begin()`). A barrier: what drew the scene is done (no pass is open here).
- Another DLSS model (`D.preset_dirty`): features are created with it, so every size gets a new one.
- Before the bloom the input is the scene's light before the exposure and the tonemap, at the game's upscaler the
  tonemapped picture: a history built from one does not hold for the other (`prebloom_run()`).
- The upscaler (`use_fsr()`) is read once: `vk_dlaa_frame()` runs between replays under the submission lock
  (`runtime/vk/vk_backend.c` `frame_end()`: at the present, first thing in the next submission, or on the present thread
  before the frame's blit), but at the present with `NX2SARIA_PRESENT_ORDER=0`, where it can clear `fsr_frame` while
  this replay is between its checks, which would send the rest of this function to DLSS's path without NGX after all.
- The size whose DLSS feature this frame runs on: its own, or the DLSS mode's while that one takes this render size
  (`range_host()`). On another size's feature DLSS reads a sub-rectangle of this frame's render size from each input.
  The light target, the motion vectors and the masks are exactly that size (`size_for()`, `mask_pass()`) and
  `range_host()` has checked it against the size the feature was created for; the depth is the light target's size by
  the arming (`vk_dlaa_pass()`), and a depth that did not cover the sub-rectangle keeps the frame on its own feature.
- Another render size on the feature the frame before ran on: DLSS's history carries on. What this size keeps of its own
  last frame, the characters' mask and the distances (`mv_pass()`), is from another moment.
- Another size on another feature: its history is from another moment, a reset. The same size on another feature (the
  DLSS mode changed): a reset too.
- Neither upscaler runs this frame: nothing is recorded, and `vk_dlaa_frame()` turns DLAA off at the next present.
- `NX2SARIA_DLAA_JITTER_TEST=<x>,<y>` (test, read once): the upscaler is told the jitter the scene was drawn with times
  these two factors. NVIDIA's DLSS guide gives no sign for the offsets against the projection's shift and says to find
  it by trying (8.4.1: its development library negates, halves or doubles an axis by a key): the picture of a still
  scene is sharp and stands still only with the right ones. The scene, the motion vectors and frame generation keep the
  real jitter.
- The edges are filled (`edge_fill()`) before the motion vectors read the depth.
- `NX2SARIA_DLAA_MV=0`: zero motion (test).
- No camera: the history cannot be moved along (2: kept as it is for a short gap, `mv_pass()`, `dlaa_cam_keep`).
- NGX records into the command buffer itself: the recorder thread must have made every call so far (`rec_drain()`).
- The free camera's probe (`fcam_probe()`) reads `mv_pass()`'s distances, behind its barrier.
- FSR 3.1 or FSR 4 (`runtime/vk/vk_ffx.cpp` `vk_ffx_upscale()`) in DLSS's place: the same images, then the same blit. A
  frame without a camera gives FSR its fixed values.
- FrameTimeDeltaInMsec is taken once: the evaluation can run a second time (on the frame's own feature, after a
  refusal).
- The feature is made for the size of its entry: this frame's own, or the larger one of the DLSS mode's, which was
  released since (`release_kept()`) and takes this frame's size as a sub-rectangle.
- IsHDR | MVLowRes | AutoExposure: linear scene light, motion vectors at the render size, no exposure texture.
- The model per mode (`NVSDK_NGX_DLSS_Hint_Render_Preset`: 1 A ... 11 K, 12 L, 13 M), as NVIDIA recommends for DLSS 4.5
  (310.x): K (transformer) for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance. M and L cost more
  on cards without FP8 (RTX 20/30). `dlss_preset` (`D.preset`, `preset_check()`) takes one model for every mode instead.
- `NX2SARIA_TRACE=perf`: the create alone; the wait and the releases of `release_kept()` have marks of their own.
- Refused while features let go of are still in flight (`release_kept()`, `retire_size()`): their video memory, or NGX's
  refusal of a feature with the parameters of a live one. The GPU finishes with them, they go, and the creation is tried
  once more: the only wait left on this path, for those features, not for the device.
- The feature's fence is the submission being recorded: NGX records the creation into cb, and each evaluation too (a
  refusal as well, for all this side knows).
- `NX2SARIA_DLSS_RANGE`: `range_note()` says which other render sizes the new feature runs.
- One bias mask: the reactive one (`NX2SARIA_DLSS_REACTIVE`, a test) where it ran, else the characters' (`mv_pass()`),
  else none. The parameter object is reused, so it is cleared explicitly.
- On the feature of a larger size, the render size is a part of what the feature was created for: it starts at the top
  left of the light target, the depth and the motion vectors, as it does in the bias mask.
- The time since the previous evaluation, which DLSS weighs its history by; 60 fps before the first and after a gap (a
  pause, a load), where no history carries over anyway.
- DLSS refused this render size on the feature of another, inside the range it reported (`range_note()`). Every size has
  its own feature from here on, as with `NX2SARIA_DLSS_RANGE=0`, and this frame runs again on its own, which starts a
  new history.
- The timing's mark after DLSS goes through the recorder, after NGX's own commands.
- Frame generation gets the same new start with this frame's inputs (`vk_dlaa_fg_inputs()`): a frame DLSS starts a new
  history on (a camera cut, another scene size or target) has no motion from the frame before that holds. Another scene
  size is one for frame generation also where DLSS's own history stayed (`D.stepped`): the depth and the motion vectors
  it reads are of that other size.
- Where FSR 3.1 made a scaled picture (not FSR 4), it is sharpened once more (`fsr3_sharpen()`).
- `NX2SARIA_MV_CHECK` (test) gets the output (`runtime/vk/vk_mvcheck.c` `vk_mvcheck_output()`).
- The result into the scene target (DLAA: the bloom, the post passes and the HUD read it and draw over it) or into the
  scaling pass's target (the HUD draws over it).
- `NX2SARIA_DLAA_MVDEBUG=1` (test): the motion vectors in place of the picture, x in red and y in green, render pixels
  as scene light (the post passes still run over them).
- `NX2SARIA_DLAA_MASKDEBUG=1` shows whichever bias mask DLSS got: the reactive one, or the characters'.
- With another destination than the light target, `vk_dlaa_swap_img` and `vk_dlaa_swap_view` are set: the scaling draw
  reads DLSS's picture.
- Frame generation keeps the HUD out of its generated frames; before the bloom, DLSS's output is not yet the picture and
  the HUD-less one is taken at the game's upscaler (`vk_dlaa_upscaler()`).
- NGX bound pipelines and descriptor sets of its own: the next draw binds everything again (`vk_exec_draw_new_cb()`).
- The depth and the motion vectors are kept for frame generation at this frame's present (`vk_dlaa_fg_inputs()`).
- The status line is written at the first evaluation, at the 300th (one early line with the frames counted) and every
  3600.

### `hudless()`, `hudless_from()`

The HUD-less picture (`vk_dlaa_fg_hudless()`, made while `vk_dlaa_hudless_wanted`): DLSS's output, which the HUD is then
drawn over, through the encode the game's last pass gives its picture (linear to sRGB), as RGBA8 UNORM bytes like the
picture the present reads. With HDR (`NX2SARIA_HDR` = 1) the game's picture stays float and linear up to the present,
which encodes it to PQ (`runtime/vk/vk_hdr.c` `vk_hdr_encode()`): DLSS's output is kept as RGBA16F for the same encode.
The game's own HDR (native) encodes inside the game, which this cannot follow: no HUD-less picture there.

`hudless()`: `dst` is the target DLSS's picture goes to, NULL for the light target itself (DLAA in place).

`hudless_from()`: the same from any picture of that kind: `src` is the picture without the HUD, linear, w x h, in
GENERAL, a format that can be blitted from: DLSS's output where the upscaler ran at the game's upscaler, the game's
upscaler's output where it ran before the bloom (`vk_dlaa_upscaler()`). It takes a level and a layer of `src` as well:
the game's own scaled picture where frame generation runs without an upscaler (`fg_only_frame()`).

- A picture of another size is retired (`retire_hl()`): the presents of the last frames may still read it.
- When the second image fails, the first image's view and what of the two images was made are destroyed at once
  (`hl_destroy()`).

### Two frame shapes

Two frame shapes. A deferred frame (the G-buffer, several colour targets at the scene size) arms on the light target
drawn after it and ends the scene at the first pass a quarter as wide (the bloom). The game's frame has no G-buffer: its
light target (B10G11R11 at the scene size, with the D32F scene depth) is drawn into from the first opaque pass on, half-
and quarter-size effect passes come and go in between. Measured (`NX2SARIA_DUMP`, Kokiri Forest, DLAA in a 2560x1440
picture): the last pass with the scene depth is the bloom composite into a second light target (each such pass at the
scene's size takes over the arming, with its depth); the game's upscaler (its stand-in here, `runtime/vk/vk_tonemap.c`)
then fills a light-format target without depth, at the picture's size, which the HUD is drawn into; a last draw copies
that target into the 8-bit sRGB output target, encoding only, since the game tonemaps before its post pass
(`runtime/vk/vk_tonemap.c`, `runtime/vk/vk_hdr.c`). The scene ends at the start of the pass the HUD is drawn into: DLSS
reads the armed pair and its picture goes into that target, which the HUD is drawn over and the last draw reads. With an
upscaling share that target is larger than the scene; in DLAA it has the scene's size. A frame without it (the menus)
ends at the last draw, at the 8-bit output target of the scene's size, and DLSS's picture goes back into the light
target, which that draw reads.

### `output_target()`

The output target DLAA runs at. With the game's own HDR (`runtime/vk/vk_hdr.c` `vk_hdr_native()`) the same pass, at the
same point of the frame, tonemaps into its PQ picture (`runtime/vk/vk_hdr.c` `vk_hdr_native_picture()`) instead of the
8-bit sRGB one; matching sRGB alone left DLAA off for the whole run there (no frame anti-aliased) and with it the frame
generation that takes DLAA's depth and motion.

### Before the bloom (`NX2SARIA_DLSS_PREBLOOM`)

What the upscaler gets at the game's upscaler (`vk_dlaa_upscaler()`) is the armed pair of the bloom composite: measured
on frame dumps (`NX2SARIA_DUMP`; 2880x1620 scene in a 3840x2160 picture, and 1280x720 in 2560x1440 with DLSS), the light
target holds the lit scene with its fog, transparents and lens flares as linear light up to about 160; the one
full-screen draw that reads it and the bloom's smallest target writes the second light target with the scene depth
attached, and that shader adds the bloom, applies the exposure, converts to the tonemap's colour space and ends in the
fitted filmic curve and a 0..1 clamp (`runtime/vk/vk_hdr.c` `tonemap_splice()` finds the same curve). Its output, 0..1
and linear (the post pass after it stores its square root), is what DLSS anti-aliased and enlarged: a picture already
exposed, tonemapped and bloomed, which the IsHDR and auto-exposure flags describe wrongly.

With the setting on, the upscaler runs at the start of the bloom chain's first pass instead: two light-format targets
smaller than the scene, no depth, right after the transparents (`prebloom_start()`). Its input is then the scene's
linear light before exposure and tonemap, at the render size, with the scene depth.

- Without upscaling (DLAA) DLSS's picture goes back into the light target and the game's own passes (bloom, flares,
  composite and tonemap, post pass, its upscaler at 1:1, HUD) all run on it as they are.
- With upscaling the game's passes after it are drawn at the render size, and the post pass and the game's upscaler are
  compute dispatches whose sizes the game sets, so they cannot be moved to the picture's size. What is moved
  (`vk_dlaa_retarget()`) are the render passes from the light target on: DLSS's picture goes into `PB.scene` at the
  picture's size; a pass drawing into the light target (the lens flares, additive, no depth test) draws into `PB.scene`,
  and the bloom composite into `PB.comp`, both at the picture's size without the depth attachment, with their viewports
  and scissors scaled (`vk_dlaa_viewport()`, `vk_dlaa_target_scale()`). The composite samples the light target through
  its texture coordinates, so it reads `PB.scene` in its place (`vk_dlaa_swap_img`). The bloom chain reads the light
  target at its own small sizes; DLSS's picture is scaled back into it first, so the bloom starts from the resolved
  picture and does not shimmer with the jitter. The post pass and the game's upscaler then run on the game's composite
  target, which this frame never wrote, and at the game's upscaler `PB.comp` goes over their picture, as DLSS's did
  before (`vk_dlaa_upscaler()`).

A frame without that bloom chain (menus, a loading screen) runs the upscaler at the game's upscaler as before.

The images, `PB`:

- `scene`, `comp`: DLSS's picture with the flares, and the composite's picture: B10G11R11, picture size.
- `used`: `D.presents` when last used (`idle_release()` frees them after `IDLE_FREE_PRESENTS`, 600).

### `pb_destroy()`

Destroys the picture-size images of the upscaler before the bloom (`prebloom_run()`); the device must be idle, or the
images not yet used.

### `retire_pb()`

The same for images the GPU may still use (only the replay reads them): to the deferred destruction. `PB.used` stays.

### `pb_images()`

The two images at w x h, made again for another picture size (the old ones once the GPU is past them, as the HUD-less
picture's).

`PB.used` is noted again once the new images exist, after any clear of `PB` (`retire_pb()` keeps it, `pb_destroy()`
would not): at 0, `idle_release()` takes images this frame draws into for 600 presents unused and frees them at the next
present.

### `prebloom_start()`

The bloom chain's first pass, on the game's frame: two light-format targets, each smaller than the armed light target
both ways, no depth. In the dumps (2880x1620 and 1280x720 scenes) it is the only such pass and it comes right after the
last pass of transparents into the light target; its targets are 480x270 at both scene sizes. The passes in between that
read the light target at a third or half of its size have one target, or a light target with an R8 one.

### `prebloom_run()`

Runs the upscaler at the bloom chain's first pass (`prebloom_start()`), before that pass begins. 0: not here this frame
(the picture's size not known yet, or not of the scene's shape): the game's upscaler runs it as before.

- The hook takes its turn (`runtime/vk/vk_exec.c` `vk_exec_hook_turn()`), as in `vk_dlaa_pass()`: the replay workers'
  draws read the jitter and its end.
- The evaluation goes into the light target, which the bloom and everything after it read. An evaluation that failed
  (`dl_fail()`) or did not run moves nothing to the picture's size.
- The composite samples the light target: `PB.scene`, which the lens flares are then drawn into, in its place.
- The bloom chain reads the light target itself, at its own sizes: the resolved picture, scaled back down.

### `vk_dlaa_retarget()`

Called by `runtime/vk/vk_exec.c` `vk_exec_pass_begin()`, in every replay thread, once the pass's targets are resolved
and the pass hooks ran: after the upscaler ran before the bloom to the picture's size (`prebloom_run()`), a pass drawing
into the light target draws into `PB.scene` and the bloom composite (one light-format target of the scene's size, with
the scene depth) into `PB.comp`, each at the picture's size and without its depth attachment (neither tests depth in the
dumps). Its draws' viewports and scissors follow (`vk_dlaa_viewport()`, `vk_dlaa_target_scale()`). A pass of the same
targets begun again (the pass broken by a copy) keeps what it got: its targets are not resolved again.

### `vk_dlaa_target_scale()`

The scale from the game's viewport and scissor to a pass `vk_dlaa_retarget()` moved to the picture's size; 0 for any
other pass.

### `prebloom_upscaler()`

At the game's upscaler, with the upscaler run before the bloom: notes the picture's size, and puts the composite drawn
at the picture's size over the game's upscaler's output (2), or leaves that output, which is already made from DLSS's
picture (1). The HUD-less picture for frame generation is the result either way.

- The same size but for a picture size changed since the upscaler ran: scaled then, for that frame.

### `vk_dlaa_upscaler()`

The game's own upscaler is the place for DLSS or FSR: its stand-in (`runtime/vk/vk_exec_draw.c`
`untranslated_dispatch_fallback()`) has just scaled the scene into the image the rest of the frame shows, and the
upscaler's picture goes over it there. Until this hook the upscaler ran at the pass after it (`vk_dlaa_pass()`), which
was taken for the pass the HUD is drawn into, the stand-in's output image as its target. With the HUD hidden the game
draws no such pass: the next pass is the frame's last, which encodes the stand-in's image into the 8-bit output. DLSS's
or FSR's picture was then written into that 8-bit target and the last draw drew the stand-in's picture over it: the
screen showed the jittered scene at its render size, scaled with a plain filter. On a player's save with the HUD off
that was every frame: blocks of render pixels, a picture that shook with the jitter by up to one render pixel, the
undrawn edge row in white, and the upscaler's cost for nothing; with the HUD on (every test save until then) the same
settings gave the upscaler's picture. Found by dumping the same frame of both saves (`NX2SARIA_DUMP`): three HUD passes
before the last one in one, none in the other.

The image is the stand-in's output when it has the light target's format and the scene's shape and is no smaller than
the scene (the movie network's and other tensor programs' outputs are RGBA8 or smaller). A frame without the dispatch
(`NX2SARIA_CS_FALLBACK=0`, another engine) still ends at `vk_dlaa_pass()`. `NX2SARIA_DLAA_AT_UPSCALER=0` (test): as
before.

- The picture's size is noted for `prebloom_run()`.
- `D.pre` is set only where the upscaler runs (`vk_dlaa_pass()`: before the bloom needs `dl_active()`); then
  `prebloom_upscaler()` takes the frame from here.
- An image more than 3 % from the scene's shape is not the stand-in's output.
- The hook takes its turn (`runtime/vk/vk_exec.c` `vk_exec_hook_turn()`), as in `vk_dlaa_pass()`: the replay workers'
  draws read the jitter and its end.

### `retire_idle()`

What `idle_release()` leaves after the upscaler is switched off. It frees the DLSS sizes and the HUD-less picture some
presents after the setting goes off. What it does not reach stayed for the process: the reactive mask's snapshots, the
FSR 3.1 and FSR 4 upscale contexts, and the DLSS sizes after a failure (`dl_fail()` turns the upscaler off without the
setting changing, so `D.on` stays set). They go here, `RETIRE_PRESENTS` (600) presents after the upscaler stopped
running and frame generation stopped making inputs of its own from a size (`inputs_on()`), on the replay thread, which
makes every evaluation, once the GPU is past them, as `free_size()` does for a size it replaces. Switched on again, the
upscaler makes them anew, as at its first frame in a run.

`retire_off_at` is `D.presents` when the upscaler stopped running, `retired_n` the times this gave memory back; both are
the replay thread's.

- No evaluation runs to take what the upscalers let go of (`evaluate()`): it goes here once the GPU is past it.
- With `NX2SARIA_DLSS_IDLE_FREE=0` nothing goes, as in `idle_release()`.
- While `inputs_on()`, nothing goes: the upscaler runs, or frame generation alone makes its inputs from a size
  (`D.fgo`).
- After a failure (`D.failed`), as `idle_release()` does for a size: frame generation is handed nothing of one from then
  on.

### `vk_dlaa_pass()`

- After DLSS ran (`D.done`), diagnostics only: another scene drawn into a light target in the same present interval is
  counted (not before the bloom: the flares and the composite after it are of that shape, and expected).
- `deferred`: a scene-size G-buffer was seen: the deferred shape. `gb_w`, `gb_h`: the depth of the pass that set it.
  `lit_w`, `lit_h`: the last light target armed in the game's shape that grew a side, in any frame.
- `NX2SARIA_DLAA_TEST_DEFERRED` (test): the run starts in the deferred shape, as if a 320x180 G-buffer was seen: 1 and
  the game's light target takes it back, 2 and it stays, as before that (the shape set for good).
- The game's builds carry no scene or output patch sets, so `res_sizes()` reports placeholders there: the sizes come
  from the frame itself (the light target's, and the output pass's).
- The G-buffer: several colour targets with the D32F scene depth. Only the light target drawn after it arms the frame;
  at night the game draws into the light target alone with the scene depth before the G-buffer too, and DLAA then ran at
  a small pass ahead of the whole scene, which reached the screen jittered. A pass of that shape smaller than the light
  target already armed is not one: the game draws a half-size effect into three targets with a half-size D32F depth
  after its scene, and taking that for a G-buffer switched every later frame to the deferred shape, where DLSS's and FSR
  3.1's smaller modes never found their output pass. The same pass in a frame not armed yet (a load, the first frame of
  an area) took the guard after it past it: in the 0.12.1 Windows logs a 720x405 pass under a 1440x810 scene and a 960x405
  one under 1920x810 set the deferred shape before the first scene was armed, and with it no frame without that effect
  was armed again, nor given its picture without the HUD in a picture larger than the scene: frame generation saw "no
  picture without the HUD" on every frame. A pass smaller than a light target armed in an earlier frame is not a
  G-buffer either, and a light target larger than the G-buffer that set the shape takes it back.
- Target 0 is the object-class buffer (RGBA8, the scene size): the characters' bias mask reads its green. A smaller pass
  of the same shape (640x360, seen while loading) does not replace the scene's in the same frame.
- The scene depth is D32F; a D16 target of the scene size also serves one composite pass before the transparents. A
  smaller pair never takes over an armed one: the camera rune draws a half-size RGBA16F target with a half-size D32F
  depth after the lighting (852x480 under a 1704x960 scene), and DLSS ran on that and wrote its picture into the scene's
  own light target, which left every object black. The game's light target, larger than the G-buffer that set the
  deferred shape: that pass was the half-size effect, and the frame is the game's shape again.
- At the first arming of a frame the reactive mask's copy is taken (`mask_snapshot()`): the lit scene, before the
  transparents.
- The game's shape: the output pass ends the scene and runs DLSS.
- `NX2SARIA_DLSS_PREBLOOM`: before the bloom chain's first pass instead, where there is one (`prebloom_run()`); not for
  frame generation alone.
- The output pass is the picture's size, the scene over the mode's share: at Ultra Performance (854 wide) a 960x540 post
  pass comes first, and DLSS wrote there, which left a 540-line picture stretched to the output. A frame whose scene is
  not at that share (the first presents before the scale patch takes, a dynamic resolution step) has no such pass: after
  a frame that missed DLSS the first pass wider than the scene is taken for 120 frames (`D.share_off`,
  `vk_dlaa_frame()`), so a mismatch costs one frame rather than every frame.
- Without an upscaling share (`native`) the scene is as large as the picture.
- The output pass has the scene's shape: the game redraws its 2000x2000 area map into an RGBA8 target of its own on a
  room change and on the pause menu, one colour target and no depth, larger than the scene both ways. Taken for the
  output pass, DLSS wrote the game's picture into the map, and the minimap showed that picture behind its room shapes
  until the pause menu drew the map again. A pass whose width over height is more than 3% from the scene's (sizes round
  to multiples of 8) is not the output; the ultrawide sets widen the scene with the picture, so their output pass still
  matches.
- DLSS runs: the replay workers' draws read the jitter and its end, and list the per-object motion it takes
  (`runtime/vk/vk_exec.c` `vk_exec_hook_turn()`); the G-buffer and the arming change nothing they read.
- DLAA in gameplay: the pass the HUD is drawn into is also at the scene's size, in the light format, and the game's
  upscaler (its stand-in here) has just written the picture there. Waiting for the 8-bit output pass, as in a frame
  without it, put DLSS's picture back into the armed light target (the bloom composite's), which nothing reads after the
  upscaler took it: the frame showed no DLAA at all. With an upscaling share the HUD's pass is the larger one, so a pass
  of this shape is not it and is ignored there as before.
- A deferred frame whose scene is as wide as the output: DLAA in place; else it waits for the scaling pass.
- The game's scaling pass: one colour target of the output size, no depth.

### `dlss_mode()`

`JITTER_LOD_BIAS` is -1.

`NX2SARIA_DLSS`, read at each present (the menu switches it live): the scene size of the mode
(`runtime/gfx/resolution.c`) and the texture bias that goes with it while DLSS or FSR 3.1 runs. NVIDIA's DLSS guide asks
for log2(render / output) - 1 on top of the game's own bias: the render scale's part (`res_dlss_lod_bias`) samples
textures for the output's resolution, and the -1 sharper still, since the jittered frames DLSS accumulates resolve the
detail one level finer: -1 at DLAA, -1.58 in Quality (67 %), -2 in Performance. The -1 comes only with the jitter
(`jitter_on()`): without it the same pixel samples the same spot every frame, and a sharper level would shimmer instead.
The sampler clamps the sum to the device's limit (`runtime/vk/vk_texture.c` `make_sampler()`).

- Dynamic resolution's step (`runtime/pacing/dynres.c` `dynres_override()`) is the mode while it differs from the
  setting.
- `NX2SARIA_DLSS_LOD_BIAS_TEST=<bias>` (test, read once): this bias in the place of the rule's while an upscaler runs,
  0 for none: what the rule's bias does to the picture is the difference between two runs.

### `preset_check()`

The preset, read again when a setting changed (the menu); a different one recreates the features at the next DLAA run.

### The scene's size after a setting changed

A DLSS or FSR 3.1 mode picked in the menu: the game makes its scene targets again at the new size and they start empty,
so the first frames drawn into them are wrong: the first one is lit without what the frame before it left (a dark
scene), and two frames later the scene can be black with the interface intact (Ultra Performance and DLAA). Measured by
capturing every present over six mode changes, 2026-11-10: bad frames 3 and 5 presents after the change, with every
other setting the same. `evaluate()` notes a scene of another size; `vk_dlaa_frame()` counts the presents since a
setting last changed, so a game that draws two sizes in turn holds nothing. `setting_presents`: presents left in which
that counts as a mode change. `vk_dlaa_frame()` sets and counts it down on the thread that runs the frame's bookkeeping,
`vk_dlaa_resized_take()` clears it at the present (`runtime/vk/vk_backend.c` `frame_hold()`): atomics.

### `vk_dlaa_resized_take()`

Called by `runtime/vk/vk_backend.c` `frame_hold()`, at each present (that frame's `vk_dlaa_frame()` may come one
submission later, `runtime/vk/vk_frame_owed.h`): 1 once when the scene took a new size after a setting changed. The
previous picture then stays on screen over those frames.

### `idle_release()`

What DLSS holds while nothing uses it (`NX2SARIA_DLSS_IDLE_FREE` 1 or 2, the default 1). With the upscaler switched off
in the menu, the sizes kept their images and features for the rest of the run, since only an evaluation frees a size
(`stale_sizes()`): at a 5120x2142 output that is the output, sharpening, mask, class and distance images, about 0.4 GiB,
and about 0.5 GiB per feature. With frame generation off and DLSS on, the HUD-less picture stayed the same way. After
`IDLE_FREE_PRESENTS` (600) presents without their use (`off_at` is `D.presents` when DLSS went off) they go here, on the
replaying thread that makes every other NGX call, without waiting for the GPU: the last evaluation that used them is
that many presents back, and the CPU is never more than a few ahead. A feature made later is made with none alive, the
case `release_kept()` leaves clean. What it costs: switching DLSS on again more than `IDLE_FREE_PRESENTS` presents after
switching it off makes the size's images and feature again, which its first evaluation waits for, as at the first frame
DLSS ran in a run.

- The images of the upscaler before the bloom go when that was off, or nothing was upscaled, for that long.
- Frame generation without an upscaler (`fg_only_frame()`) makes its motion images in the size of the scene's size at
  every frame, and that size can change from one frame to the next: a size it used within the last `IDLE_FREE_PRESENTS`
  evaluations may still be in flight, and `destroy_size()` does not wait for the GPU. Such a size stays, and so does the
  last one; a DLSS feature made for the same sizes (DLAA) goes, since the upscaler that used it has been off for that
  long.
- Frame generation's motion vectors were a size's (`evaluate()`); none is handed out this late (`fg_current()`). Without
  an upscaler they are the kept size's, which this frame's inputs may still be.

### Frame generation without an upscaler

Frame generation (`runtime/vk/vk_dlssg.c`, `runtime/vk/vk_fg.cpp`, `runtime/vk/vk_fg4.cpp`,
`runtime/gfx/gfx_present.c`'s NGX presenter) takes the frame's depth, motion vectors, camera and picture without the HUD
from this file. With an upscaler they are what its evaluation used (`evaluate()`). With frame generation running and no
upscaler (dlaa = 0, or DLSS set where it is not available: `D.fgo`, `vk_dlaa_frame()`) the frame is armed and ended in
the same places (`vk_dlaa_draw()`, `vk_dlaa_pass()`, `vk_dlaa_upscaler()`), and `evaluate()` hands it to
`fg_only_frame()`, which leaves the upscaler's evaluation out:

- The camera is the scene context the scene's draws use (`vk_dlaa_draw()`), and the motion vectors come from the same
  pass at the scene's size (`mv_pass()`, the per-object motion included), into a size's images made for the scene's size
  alone: no DLSS feature, and the image the upscaler's picture would go to has the scene's size;
- Nothing is jittered: `next_jitter()` is never called, so `jit_ok` stays 0 and no viewport moves
  (`vk_dlaa_viewport()`). A jittered frame that nothing resolves shakes on screen. The vectors and frame generation get
  a jitter of 0;
- The picture without the HUD is copied from the target the upscaler's picture would have gone to, which the game's own
  scaling (`runtime/vk/vk_exec_draw.c` `untranslated_dispatch_fallback()`, `runtime/vk/vk_tonemap.c`) has just written
  at the picture's size, or from the light target itself where the frame ends in place. A deferred frame's scaling pass
  (`vk_dlaa_pass()`) draws its picture inside the pass that ends the scene, so its target does not hold it yet: such a
  frame has none;
- Nothing is drawn over the frame: the game's scaling draw stays (`D.up` 0) and no pass reads another picture
  (`vk_dlaa_swap_img`).

The scene can then be smaller than the picture (the game's own render scale), as in the upscaler's scaling modes: the
depth and the vectors have the scene's size and the picture without the HUD the picture's, which every frame generation
takes as it does there (`runtime/vk/vk_dlssg.c` `vk_dlssg_frame()`: the vectors scaled by the depth's size, the output
by the swapchain's; `runtime/vk/vk_fg.cpp` `vk_fg_frame()`: the depth's size as the render size, the context made for up
to the picture's; `vk_ngxfg_evaluate()` here: the vectors' and the depth's own rectangles; `runtime/gfx/gfx_present.c`
`hudless_record()`: the picture without the HUD scaled to the swapchain like the picture).

### `fg_runs()`

Frame generation that takes this frame's inputs: NVIDIA's through Streamline (`runtime/vk/vk_dlssg.c`, set up with the
device), AMD's swapchain in place (`runtime/vk/vk_fg.cpp`, either library), NVIDIA's through NGX or AMD FSR 3.1's
through the runtime's passes (`runtime/vk/vk_fsr3fg.c`), set now (`vk_ngxfg_on()`, `vk_fsr3fg_on()`).

### `fg_off_why()`, `fg_state_line()`

One line at the first present and one at each change: whether frame generation runs, and where its inputs come from.
With none running, the line says why. It said only "no inputs are made" before, the same on a card where the one set
cannot run as with the setting off. The key `fg_state_line()` compares, from `fg_off_why()`:

- 0: the setting is off.
- With dlss: 1 the card in use has no DLSS Frame Generation (`runtime/vk/vk_dlssg.c` `vk_dlssg_why()`: another vendor's,
  or Streamline found no adapter with it, as on an RTX 30) and AMD FSR 3.1's runs here; 2 the same where FSR 3.1's does
  not; 3 Streamline gave another reason (`*sl_why`); 4 it has not started, for a reason its own lines give (the files,
  the device, NGX on Linux).
- With fsr3: 5 it does not run here; 6 its swapchain is not made yet.
- dlss set in the menu while the game runs starts only at the next start: Streamline is set up with the device
  (`vk_dlssg_wanted()` reads the setting once) and NGX's frame generation needs DLSS upscaling on at the start
  (`vk_ngxfg_wanted()`), so neither has lines of its own yet. Then 7 this card cannot run it even then
  (`vk_dlssg_gpu_may()`) and AMD FSR 3.1's runs here; 8 the same where FSR 3.1's does not; 9 the card may run it.

### `blit_from()`

`vkCmdBlitImage` reads an image of format f with optimal tiling into `hudless_from()`'s float, UNORM or sRGB images: the
format must have `BLIT_SRC`, and an integer format may only be blitted into another integer one, which linear filtering
(no integer format has it) rules out. The upscaler's targets (B10G11R11, RGBA16F, RGBA8) have all of that by the Vulkan
spec's required formats; anything else is asked once per format.

### `fg_only_frame()`

- `D.up` stays 0: the game's scaling draw makes the picture (`vk_dlaa_skip_draw()`).
- A barrier: what drew the scene, and the game's scaling into `dst`, is done (no pass is open here).
- Another size: its own distances and masks are from another moment, and the vectors start anew.
- `NX2SARIA_DLAA_MV=0`: zero motion (test), as in `evaluate()`.
- No jitter, and bias 0: the bias mask is DLSS's alone.
- No camera: frame generation starts anew.
- The free camera's probe (`fcam_probe()`) reads `mv_pass()`'s distances, behind its barrier.
- The motion pass bound a pipeline and descriptors of its own: the next draw binds all again (`vk_exec_draw_new_cb()`).
- `D.jdepth` and its size are noted: the scene depth the camera's draws go to (`vk_dlaa_draw()`).

### `vk_dlaa_frame()`

- `D.presents` has one writer; `presents_now()` reads it unlocked.
- `setting_presents` is counted down to 0, not past it: `vk_dlaa_resized_take()` may clear it meanwhile.
- The state of the upscaler before the bloom is cleared: the frame before was recorded up to its present.
- `NX2SARIA_DLSS_PREBLOOM` is live: the evaluation point changes at the next frame.
- While NGX is not up, FSR 3.1 follows the setting.
- Frame generation without an upscaler takes this frame's inputs alone (`fg_only_frame()`); switching in either
  direction starts anew as switching the upscaler does, and the frame before gets nothing to frame generation.
- When the upscaler goes off, `off_at` is set: `idle_release()` counts the upscaler's sizes as unused from there.
- Either way the next DLAA frame starts a new history from an unjittered frame.
- Frame generation gets nothing made before the switch (`fg_current()`).
- After a failure nothing is armed or counted any more (`inputs_on()`): those frames are gameplay without inputs.
- A frame armed that missed DLSS: `vk_dlaa_pass()` picks the output pass without the mode's share for 120 frames.
- The first few after DLSS first ran: what the frame drew, to tell a frame drawn another way from a loading screen.
- A frame jittered but not resolved shook: the jitter is held for 60 frames.

### The game's FXAA (`NX2SARIA_FXAA`, live)

The game's FXAA is two full-screen passes after the bloom, just before the HUD: one copies the scene target into a
scratch target, the other draws the anti-aliased result back over the scene (found by diffing frame dumps with the
game's own switch, ModelSceneExtensionParam EnableFXAA, on and off). With fxaa = 0 the draws of that second pass's
fragment shader are skipped, so the scene reaches the HUD as it was. Shaders are known by the FNV-1a hash of their
SPIR-V, as `runtime/vk/vk_dump.c` names them; a translator change gives new hashes and the switch then does nothing.
With a scene smaller than the output (the game's own 5/6 render scale, or a DLSS mode) the same shader draws the scaled
picture into the output-sized target, in a pass without the depth attachment the in-place FXAA has: there it is always
drawn, whatever fxaa says (skipping it would leave the 3D picture out), unless DLSS already wrote that target this
frame.

`FXAA_FS` holds the stock translator's hash, the one with `tools/xlate/` patch 0010 (PCSS, PR #89) applied, the one with
patches 0010 and 0011 (the shadow filter spread) applied, and that one with the `zero_locals` job flag (Mesa,
`runtime/vk/vk_caps.c`). A scaling draw this list misses still scales DLSS's picture, not the jittered scene: after DLSS
ran, passes larger than the light target sample DLSS's output (`vk_dlaa_swap_img`).

### `spirv_hash()`

- The hash is kept by the shader (`gfx_shader_spirv_fnv()`): a compressed one is not decoded on this thread.
- A full table gives 0: no skip.

### `vk_dlaa_skip_draw()`

- `fxaa = 0` reports once whether it found the game's FXAA draw: the first skip, or a minute of frames (3600) with none,
  which is what a translator change that moves `FXAA_FS` looks like on every GPU, with or without DLSS.
- The setting is read once a frame: the menu switches it live.
- With nothing to skip there is no shader lookup per draw at the defaults.
- A pass without depth is the scaling pass, which DLSS replaced.

### DLSS Frame Generation through NGX

`runtime/gfx/gfx_present.c` records this into the present command buffer once the frame's picture, HUD included, is in
bb. It generates count frames between the previous call's picture and this one into `out[0..count-1]` (MultiFrameIndex 1
to count) and a copy of bb into real; `runtime/gfx/gfx_present.c`'s presenter shows them in that order, then the
rendered one. The inputs are the ones the Streamline path gets (`runtime/vk/vk_dlssg.c`): the depth, motion vectors,
jitter and camera of the frame DLSS last ran on, and the picture without the HUD. NGX comes up only with DLSS upscaling
on at the start (`vk_ngxfg_wanted()`); switched off in the menu after that, the frames armed for frame generation alone
give the same (`fg_only_frame()`, no jitter).

### `vk_ngxfg_evaluate()`

- A frame without its inputs: the next generation has no valid previous frame (`FG.reset`).
- "DLSSG.MvecScaleX" and "Y" are 1 over the size: the vectors are in render pixels.
- The camera planes are the frame's own (`proj_planes()`), as `runtime/vk/vk_dlssg.c` gives them.
