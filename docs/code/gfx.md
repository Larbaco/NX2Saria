# runtime/gfx

The graphics front end between the game's NVN calls and the Vulkan back end: the interface both sides share
(`runtime/gfx/gfx.h`), the shader translator's client and its caches (`gfx_shader.c`), the window, the swapchain, the
present and the steps of leaving the game (`gfx_present.c` with a window back end, SDL or Win32), the resolution
patches (`resolution.c`), the mesh-shader fallback (`mesh_fb.c`, `mesh_emu_spirv.c`, `mfb_batch.h`, `mesh_fb_hold.h`),
the water shader recipe (`spirv_water.c`), the texture decoders (`tex_astc.c`, `tex_astc_mt.c`, `tex_deswizzle.c`), the
null back end (`gfx_null.c`) and six probes.

What the Switch's GPU and NVN do that a port must reproduce: [maxwell.md](../maxwell.md). Where the area sits in the
host: "4. Graphics" in [architecture.md](../architecture.md). The feature docs the notes below point to:
[water-projective.md](../water-projective.md) (the water recipe), [wide-hud.md](../wide-hud.md) (the interface on a
wide picture), [frame-generation.md](../frame-generation.md), [presets.md](../presets.md) (render scale and dynamic
resolution), [performance.md](../performance.md) (stalls, the hang watch, background builds),
[shader-delivery-research.md](../shader-delivery-research.md) (why shaders are prepared on the player's PC),
[controls.md](../controls.md) (the window keys). The log lines of the area: [logging.md](../logging.md). The checks
that drive this code: [building.md](../building.md#checks). The back end's side is on the `runtime/vk/` pages, the
front end's on [nvn.md](nvn.md).

In the notes below "the game" is the title the build is for, by version (1.1.0, 1.1.1, 1.2.1, 1.4.3); "Ocarina" is
1.1.1, whose driver is NVN2. `main+0x...` is an address in the game's main module. "The translator" is the shader
translator of `tools/xlate/`, run as a child process; "c1" and "c2" are the two control blocks a program carries
besides its code, and a "c2 slot" one of the 32 texture or image slots the second one names.

A file whose first lines are a licence notice keeps them in the code; the rows of `THIRD_PARTY_NOTICES.md` say what
each notice covers.

## `runtime/gfx/gfx.h`

The interface of the graphics layer: the window, the shader service, the back ends and the NVN front end.

- `oe_push_message()` and `oe_set_focus()` are in `runtime/hle/ov_system.c` (`oe_set_focus` feeds
  GetCurrentFocusState; `NX2SARIA_ASSUME_FOCUS` pins it to 1); `ov_nv_state()` is in `runtime/hle/ov_gfx.c`.
- `RT_GPU_VA_BASE` (0x8000000000, 2^39) starts the GPU address window for pools the CPU cannot see; it is never mapped
  on the host. `RT_GPU_VA_SIZE` is 64 GiB.

### Window, vsync and focus

Implemented by `runtime/gfx/gfx_present.c`, the window back end (`runtime/gfx/gfx_wsys.h`) and
`runtime/vk/vk_device.c`. Host threads only; they never run guest code.

- `gfx_window_start()` starts the window thread, the Vulkan device and the swapchain; calling it again does nothing.
- `gfx_tl_note()` and `gfx_tl_present()` belong to `NX2SARIA_GPU_TIMELINE=<frame>` (the frame timeline of
  `runtime/vk/vk_sync.c`): `gfx_tl_note` records an event of the frames the timeline records, at CPU time `t` (0: now)
  with two values; `gfx_tl_present` counts the presented frames and writes the record to the log when it is complete.
- `gfx_vsync_bind()` takes the `nn::vi` vsync event, signalled at 60 Hz; `gfx_vsync_ticks()` counts ticks since the
  start; `gfx_vsync_time_ns()` is the `kern_now_ns()` time of a tick (pacing).
- `gfx_window_refresh_hz()`: the refresh rate of the window's monitor, 0 when unknown. `gfx_window_hosted_ns()`: the
  `kern_now_ns()` of the newest host present, 0 before one.
- `GfxLockHolders` and `gfx_lock_holders()`: who holds the present, queue and window-start locks of `gfx_present.c`,
  read without taking them (the hang watch).
- `gfx_window_in_front()`: the window has the focus and is not minimized, so the game is expected to present.
- `vk_hdr_native()`: `NX2SARIA_HDR = native`, the game's own HDR picture (`runtime/vk/vk_hdr.c`).
- `gfx_picture_rect()`: where the last present put the picture, as fractions of the swapchain (x, y, w, h); 0 before
  one.

### The shader service client

Implemented by `runtime/gfx/gfx_shader.c`.

- `GFX_STAGE_TS` and `GFX_STAGE_MS` are NVN2's task and mesh programs (VK_EXT_mesh_shader). The `GFX_TEX_*` values
  follow the order of `tex_names` in `gfx_shader.c`.
- `GfxShaderAnswers` holds the translator's answers (the `Accessor` of `tools/xlate/Program.cs`): `tex_type`,
  `img_type` and `img_format` per c2 slot, 0 meaning not asked; the types are `GFX_TEX_*` and `img_format` is the
  translator's TextureFormat. `attr_type` and `frag_out_type` are `GFX_TYPE_*`. `mvprev` (vertex shaders): also write
  last frame's position (the translator's patch 0015, `runtime/vk/vk_mvprev.c`); it is packed as bit 7 of the
  `depth_minus_w` byte.
- `GfxShader` is the SPIR-V words plus the parsed reflection; the cache owns it.
- `gfx_shader_get()` blocks, and returns NULL on a failure (counted).
- `gfx_shader_get_async()` is `gfx_shader_get` with c1 inside the code, but a shader in neither cache is queued to a
  background translation and NULL is returned with `*pending = 1` until it is ready. A failed translation stays failed
  (`*pending = 0`).
- `gfx_shader_await()` is `gfx_shader_get_async`, but a shader queued for translation is moved to the front of the
  queue and waited for, `wait_ms` at most (`UINT32_MAX`: until it is done). NULL when the translation failed, or with
  `*pending = 1` when it was still running at the end of the wait: it stays at the front of the queue.
- `gfx_shader_async_done()`: translations finished by the background worker. `gfx_shader_counts()`: distinct keys in
  memory and translations done (`NX2SARIA_TRACE=perf`). `gfx_shader_disk_counts()`: disk cache reads on the render
  thread (count, ns) and by the loaders (`load_budget` in `gfx_shader.c`).
- `GfxShaderRam` and `gfx_shader_ram()`: the shader caches' host memory for the memory report (`system_log()` of
  `runtime/vk/vk_memory.c`), at report time, in bytes: the memory cache of translated shaders (`shaders` entries), the
  use masks, the cache pack's index, the play records' sets and pending appends. The memory cache's SPIR-V ("SPIR-V at
  rest" below): `spv_held` bytes held, plain copies and compressed frames together, against `spv_full` uncompressed;
  `spv_packed` shaders held compressed alone; `compress` 1 on, -1 off, 0 no shader yet. Then the decodes that readers'
  pins made, and those on the drawing threads (count, time, longest).
- `gfx_shader_set_host()`: the translator's answers for this device (`runtime/vk/vk_caps.c`): shaderFloat64, the
  storage offset rounding, the subgroup size per vertex, fragment and compute stage.
- `gfx_shader_key()`: the 32-byte cache key, stable while the translator is. `gfx_shader_input_id()`: the 32-byte
  input id, stable across translators.
- `gfx_shader_origin()`: where the translation in use came from: 0 the shader cache, under the current translator; 1
  the shader cache, a previous translator's ("Carry" below); 2 translated in this process, so nothing was prepared from
  it.
- `gfx_shader_identity()`: the translator's base identity (before the host options are mixed in; the one
  `identities.txt` lists): 32 bytes, all zero when there is no translator. `runtime/vk/vk_pipeline.c` stamps the
  pipeline cache files with it (`cache_stamp_check`).
- `gfx_shader_carries()`: 1 when the shader cache's carry is on and lists the translator id among the previous ones it
  holds translations of. A preparation carries nothing whatever this says (`C.suspended` in `gfx_shader.c`).
- `gfx_shader_load_input()`: by input id from memory or disk; `xlate` 1 also translates it from its stored inputs.
  NULL if absent.
- `gfx_shader_prewarm()`, the boot prewarm: `out[i] = gfx_shader_load_input(iids[i], 1)` on `threads` threads with up
  to `procs` translators, then every other stored input not cached under this translator is translated (one that a
  previous translator's translation serves is left to `gfx_shader_carry_start`'s pass). It logs progress and the total.
- `gfx_shader_carry_start()`, after the boot prewarm of a game run (`runtime/boot.c`): when the shader cache holds
  translations of a previous translator (`NX2SARIA_SHADER_CARRY`), it starts the background pass that translates them
  again under the current one for the next start and then drops the old ones.
- `gfx_shader_prewarm_progress()`: how far the running (or last) `gfx_shader_prewarm` is: shaders done of total, and
  how many of them it translated (the rest were cached). Any thread; the numbers lag the workers by at most 100 ms.
- `gfx_present_boot()`: a black frame with the boot screen (`menu_boot_frame` of `runtime/ui/menu.cpp`) over it:
  `boot.c`'s wait for the prewarm, on the boot thread before the guest runs (nothing else presents then).
- `gfx_shader_record_at()`, answers met in play: from this call on, every (program, answers) a `gfx_shader_get*` call
  resolves is appended to `shader-answers.bin`, and its program to `shader-programs.bin`, in the directory of
  `keys_path` (`pipeline-keys.bin`). `gfx_shader_record_flush()` appends the pending records; without `force` at most
  every 2 s.
- `gfx_shader_prepare_recorded()` translates every recorded (program, answers) on `threads` threads with up to `procs`
  translators (`NX2SARIA_TEST_PREPARE_SHADERS`).
- `gfx_shader_prepare_seed()` is `nx2saria.exe --prepare-shaders`: it translates `<dir>/seed-answers.bin` and
  `seed-programs.bin` (`tools/prepare_shaders.py`) and writes `<dir>/shader-prepared.bin` (stage, priority, input id
  per variant). 0 on success.
- `gfx_shader_guess_refine()` (`runtime/gfx/gfx_shader_guess.c`): the answers of a prepared default variant, read from
  its first translation `s`: vertex inputs the shader reads as integers become uint, a compute shader's textures and
  images take the types its translation declares. 1 when `*out` differs from `*a` (translate again with `*out`).
- `GfxWarmEnt`: `prio` 0 is met in play, 1 to 3 prepared. `gfx_shader_warm_list()` returns a malloc'd list, recorded
  first, then prepared; it needs recording on.
- `gfx_shader_load_private()`: a disk-cache load the caller owns (`*owned` 1: release it with
  `gfx_shader_release_private()`), or the memory cache's entry (`*owned` 0). NULL if not cached.
- `gfx_shader_spirv_pin()` and `gfx_shader_spirv_unpin()`: a shader's SPIR-V may be held compressed while nobody reads
  it. A reader pins it: the pin returns the plain SPIR-V (decoded first if need be), valid until the matching unpin;
  every pin needs its unpin, and the pointer is not used after it. Any thread.
- `gfx_shader_words()`: the SPIR-V's length in words, without reading it.
- `gfx_shader_spirv_fnv()`: FNV-1a (64-bit, over the bytes) of the SPIR-V, worked out once per shader and kept, so it
  decodes nothing on a compressed shader (`NX2SARIA_SKIP_SHADERS`, `runtime/vk/vk_dump.c` and `runtime/vk/vk_dlaa.c`
  name shaders by it).
- `gfx_shader_bindings()`: the values of the module's OpDecorate Binding instructions, in module order, in `*b` (kept
  with the shader).
- `gfx_shader_res_json()`: the translator's `.res.json` text. `gfx_shader_write_code()`: the guest program, from the
  stored inputs.
- `gfx_shader_own()`: a shader of the runtime's own (SPIR-V and a `.res.json` text as the translator writes them),
  outside the cache and never freed; its key is the sha256 of both. NULL when the SPIR-V is malformed.
- `vk_hdr_shader_swap()` (`runtime/vk/vk_hdr.c`): with `NX2SARIA_HDR=1`, splices the HDR tonemap into a fragment
  shader's SPIR-V (malloc'd, `*words` long) when it is the game's tonemap, and unclamps the game's FSR pass;
  `gfx_shader.c` calls it for every fragment shader entering the cache.
- `gfx_shader_failures()`: failed translations; it feeds `GfxStats.shader_failures`.

Reflection is parsed once per shader, from the translator's `.res.json` and the SPIR-V, before the shader enters the
cache. The fields of `GfxShaderRes`:

| Field | Meaning |
|---|---|
| `set`, `binding` | as the SPIR-V decorates them: set 0, bindings from 1 per shader (0 is the support buffer) |
| `slot` | UBO: `c[slot]`; SSBO: the storage buffer index; TEX: the c2 combined slot (handle at 0x20 + 8 slot) or, separate, the texture slot (0x168 + 8 slot); IMG: the image slot (0x120 + 8 slot); -1: other |
| `type` | TEX and IMG: the translator's SamplerType: 1 1D, 2 buffer, 3 2D, 4 3D, 5 cube, with 0x100 array, 0x200 multisample, 0x400 shadow |
| `format` | IMG: the declared format as a `GfxShaderAnswers.img_format` code, 0 unknown |
| `flags` | SSBO: 1 written; TEX and IMG: the translator's TextureUsageFlags (4 image store, 8 coherent) |
| `cbuf_slot` | TEX and IMG: the constant buffer of the handle (-1: the bound texture buffer); SSBO: of its address |
| `handle` | TEX and IMG: the raw handle (a word offset; separate: texture, with the sampler shifted left by 14); SSBO: the address offset |
| `count` | descriptors (the array length) |
| `separate` | TEX: the texture and sampler handles are separate |
| `pool` | TEX: 1 the texture pool, 2 the sampler pool, indexed by bindless handles (set 2) |
| `ptr` | SSBO: 0, or 1 + the byte offset of this buffer's address inside the storage buffer that `cbuf_slot` and `handle` name (a pointer table; the translator's patch 0022, `"ptr"` in the `.res.json`) |
| `exact` | TEX: the module fetches its texels by position or uses its size or level count, so it needs every level of the texture as the game made it (`reflect_exact` in `gfx_shader.c`) |

- `GfxShaderIo`: `GFX_TYPE_*`, the components, and `packed` 1 with a Component decoration.
- `gfx_shader_stage()`: `GFX_STAGE_VS`, `GFX_STAGE_FS` or `GFX_STAGE_CS`. `gfx_shader_resources()`: the count of the
  `GFX_RES_*` kind, the array in `*res`.
- `gfx_shader_exact_seen()`: shaders loaded so far with a `GfxShaderRes.exact` texture (the texture census line).
  `gfx_shader_pool_short_seen()`: shaders in memory whose texture pool array has the earlier length, 32768
  (`pool_short_note` in `gfx_shader.c`; the warning of `vk_texture_register`).
- `gfx_shader_slot_mask()`: the texture and image slots whose type its translation read.
- `gfx_shader_inputs()`: the Input Locations (vertex attributes, fragment varyings) as a mask.
  `gfx_shader_outputs()`: the Output Locations (vertex varyings, fragment colour targets).
- `gfx_shader_omap()` (fragment): the colour components it writes, 4 bits per target (the shader header's omap); 0
  unknown. `gfx_shader_mvprev_banks()`: for a last-frame variant, the banks (bit n: `cn`) its position reads.
- `gfx_shader_palette()` (vertex): the matrix palette its skinning reads (`reflect_palette` in `gfx_shader.c` states
  the rule). 0: none. Else the bytes of one bone matrix (48 or 64), `*slot` the uniform bank `c[slot]` it is read from,
  or with `*storage` 1 the storage buffer index, and `*joints` the input Locations whose integer values pick the
  matrices.
- `gfx_shader_local_size()` (compute): the LocalSize execution mode; 1, 1, 1 otherwise.

### `GfxBackend`

The back end (`runtime/gfx/gfx_null.c` and the `vk_*.c` files), called by the NVN front end (`nvn_api_*.c`,
`nvn_ext.c`) on guest threads. `gfx_backend()` picks it: `NX2SARIA_GPU=null|vulkan`, default vulkan.

- `pool_init`: 0 fails MemoryPoolInitialize. `pool_map_virtual` gets `NVNmappingRequest[n]`, 0x28 bytes each.
- `submit` replays, and returns this submission's fence value (`gfx_submit_queue` names the game's queue).
- `fence_wait`: 1 when reached; the caller has given its core up.
- `exec_wait`: as `fence_wait`, but only until the GPU has executed the submission's own commands: its write-backs to
  guest memory and its fence's side effects may follow (`exec_tl` in `runtime/vk/vk_sync.c`). The same as `fence_wait`
  where there is no split.
- `game_wait`: the game's own wait for a fence (nvnSyncWait): `fence_wait`, or, where the back end knows the rest of
  the value's GPU work reads nothing the game can change and hands it nothing, until its results are there
  (`runtime/vk/vk_sync.c` `vk_sync_game_wait()`).
- `fence_done`: the highest value whose side effects are written.
- `present` queues the present and returns at once; the host present goes out at `at_ns` (the `kern_now_ns` clock),
  at once when that has passed or `at_ns` is 0.
- `frame_start`: the game starts a frame (nvnWindowAcquireTexture in `runtime/nvn/nvn_api_obj.c`), after the image's
  own waits (the pacer's hold and the GPU's): the back end may hold it longer (`vk_dlssg_frame_start` in
  `runtime/vk/vk_dlssg.c`, Reflex's sleep for DLSS Frame Generation). NULL: nothing to do.
- `input_read`: the game is about to read its pad (`npad_states` in `runtime/hle/ov_input.c`), several times a frame:
  where the pad read is the first thing a frame does, the back end holds the frame's start here, before the input is
  sampled (`vk_lowlat_frame_start` in `vk_lowlat.c`). NULL: nothing to do.
- `frame_paced`: nvnQueuePresentTexture, with the pacer's period in ns (0: it holds nothing; also during a movie,
  whose slots only the pacer keeps): the period the pacer still holds, for both the present's slot and the window
  image. `period_ns` while nothing else holds the limit; less when the back end holds the next frame's start to the
  period itself: the game's ceiling (`frame_step_ceiling_ns` of `frame_rate.c`) where Reflex holds it with DLSS Frame
  Generation (`vk_dlssg_pace`), 0 where a driver's low-latency sleep does (`vk_lowlat_pace`). NULL: never.
- `guest_written`: the host wrote pool storage. `guest_read`: the CPU reads pool storage, and GPU-written texels go
  there first.
- `cpu_synced`: the CPU waited for the fence: small render targets written by work up to it go to their storage
  (`vk_texture_write_back_small` in `runtime/vk/vk_texture.c`).

### The rest of the header

- The NVN front end: `nvn_procs_register()` registers every proc with `hostfn_register`, before `hostfn_seal`
  (`runtime/hle/ov_gfx.c`); `nvn_proc_address()` gives the synthetic host address of a proc, 0 for an unknown name.
- `GfxStats` is filled by `nvn_api_*.c` and `vk_*.c`; `runtime/boot.c` prints it.
- `rt_frame_presented()` is `boot.c`'s: `--frames N` ends the run after the Nth present (and saves `--shot` first,
  through `read_texels`).
- The movie overlay (`runtime/hle/ov_movie2.c` to `gfx_present.c`): `gfx_movie_frame()` hands the latest decoded movie
  frame, RGBA8, shown over the game image while frames keep coming (see `gfx_movie_frame()` under `gfx_present.c`
  below for when the overlay is on). `gfx_movie_frame_own()` is the same but takes the malloc'd buffer, freed later.
- `gfx_submit_queue`: the game's queue (the NVNqueue address) of the submission this thread is handing to
  `GfxBackend.submit`, 0 when none (probes). nvnQueueSubmitCommands sets it around the call; `runtime/vk/vk_exec.c`
  orders queues by their events with it.

## `runtime/gfx/gfx_guard.h`

Checks of the presentation and sampler paths that must not trust what they are given: the game's crop rectangle
(`present_image` in `gfx_present.c`), the swapchain image list a driver or a frame generation library returns
(`create_swapchain` in `gfx_present.c`), and how often a failure that can repeat is logged (there and in
`make_sampler` of `runtime/vk/vk_texture.c`). It is its own header, without Vulkan, so `tests/gfx_guard_check.c`
checks it alone.

### `gfx_crop_clip()`

The source rectangle of a present into `out` (x, y, width, height): `crop` is the game's (nvnWindowSetCrop: x, y,
width, height, guest values, may be NULL) on a `w` x `h` image (`w`, `h` > 0). A negative origin moves to 0 with the
extent kept, as before; an extent past the edge is cut there. The edge is compared by subtraction (`cw > w - cx`, with
`0 <= cx < w`), so no guest value overflows an int, where `cx + cw` did for an origin or extent near `INT_MAX`.

What it returns:

| Value | Meaning |
|---|---|
| `GFX_CROP_WHOLE` (0) | no crop (none given, or an extent of 0 or less): the whole image, as the game asks |
| `GFX_CROP_KEPT` (1) | the crop lies inside the image and is used as it is |
| `GFX_CROP_CUT` (2) | it reached past the image's edge (or before its origin) and was cut to the image |
| `GFX_CROP_OUTSIDE` (3) | nothing of it is inside the image: the whole image instead of an empty or inverted blit |

### `gfx_rect_inside()`

Whether a rectangle (x, y, width, height) lies inside a `w` x `h` image, compared without overflow.

### `gfx_sc_images_ok()`

Whether a swapchain's image list can be used: the count query returned `count_r` with `count` images, and the list
query (with room for `count`, at most `cap`) returned `list_r` with `got` images. The results are VkResult values: 0 is
VK_SUCCESS, 5 VK_INCOMPLETE (more images than the room given), below 0 an error. A count of 0, a count past `cap` (an
acquire could name an image the arrays have no slot for), a list query that did not succeed (VK_INCOMPLETE included) or
returned fewer images than counted: the swapchain cannot be used.

### `gfx_fail_log_due()`

A failure that can repeat every frame is logged the first 8 times and then at each power of two of its count (`n`
counts from 1): a session shows that it happened and how often, without a line a frame.

## `runtime/gfx/gfx_wsys.h`

The line between `gfx_present.c` (swapchain, present, vsync) and the window system's back end (`gfx_window_win.c` with
Win32, `gfx_window_sdl.c` with SDL). Host threads only.

What the back end provides:

- `wsys_open()` makes the window (and its thread); 0 on failure. Called once.
- `wsys_surface()`: a new Vulkan surface of the window; `VK_NULL_HANDLE` on failure.
- `wsys_client_size()`: the drawable area in pixels. `wsys_minimized()`: nothing to present into.
- `wsys_resize()`: `gfx_window_resize`, the drawable area becomes `w` x `h`.
- For `--probe-window` (`runtime/gfx/gfx_window_probe.c`), synthetic window events, handled on the window thread
  before these return: `wsys_probe_focus()` (the window gains, 1, or loses, 0, the focus), `wsys_probe_escape()` (Esc
  pressed) and `wsys_probe_click()`: a left click, 0 in the client area, 1 activating the window from its client area,
  2 activating it from the title bar. It returns 0 when the window system has no such event (then the caller skips that
  check).

What `gfx_present.c` provides the back end:

- `gfx_window_mark_dirty()`: the window changed size, and the next present rebuilds the swapchain.
- `gfx_window_focus_changed()`: the window gained (1) or lost (0) the focus.
- `gfx_window_closed()`: the player closed the window: it saves, then ends the process. It does not return, but for
  its own thread coming back (`rt_exit_begin` in `gfx_present.c`).
- `gfx_window_set_refresh()`: the refresh rate of the window's monitor, 0 unknown.
- `gfx_cfg_fullscreen()`: `NX2SARIA_FULLSCREEN`. `gfx_cfg_window_size()`: `NX2SARIA_WINDOW_SIZE`, the default when it
  cannot be parsed or is out of range.

## `runtime/gfx/gfx_null.c`

The null graphics back end: no GPU. Every submit completes at once and advances the fence value, pools always
initialise, presents are only counted, texel reads return zeros. A submit performs the CPU-visible side effects of its
op streams in order: event signals, counter reports (timestamp ticks, counter 0), buffer-to-buffer copies, buffer fills
(`OP_CLEAR_BUF`) and uniform updates. It is the CPU-side gate for the NVN
front end. The file also holds the back-end choice (`gfx_backend()`) and the statistics (`gfx_stats()`).

### `gfx_backend()`

`NX2SARIA_GPU=null|vulkan`, default vulkan (`rt_config`); chosen once.

### `null_cpu_addr()` `null_replay()`

The CPU-visible side effects of one op stream, in stream order. Everything else (draws, dispatches, clears, texture
copies) has no effect here. Addresses of CPU-invisible pools (the GPU window) hold no host memory, so those writes are
dropped.

### `null_report()`

`NVNcounterData` is {counter, timestamp}. Timestamps are Tegra 19.2 MHz ticks, the base of GetSystemTick; every counter
type the null GPU cannot measure (all but the timestamps, samples passed included) reports 0.

### `null_submit()` `null_fence_wait()`

`null_submit` sweeps the pins (`nvn_pins_sweep`) first: no replay state outlives a submission here. In
`null_fence_wait` a value never submitted is never reached.

## `runtime/gfx/gfx_abi_probe.c`

`--probe-gfx-abi`: every host object of `runtime/nvn/nvn_obj.h` fits the storage `runtime/nvn/nvn.h` reserves for its
NVN type, the layouts the game reads directly (NVNtextureView, the command buffer's inline fast-path fields) sit where
the game's NVN 55 code expects them, and the GPU address window lies above every guest region. The same facts are
static assertions in `nvn_obj.h`; this mode prints the sizes so a change shows its margin. It needs no game files.

### `check_write_watch()`

The write watch: a committed heap page and an alias page, written once, are reported once. The first
`mem_written_pages` call drops whatever the commit itself marked.

- A frozen region is read-only until the first write thaws it (the write lands and is reported); `mem_thaw` (a file
  read) thaws it and holds it thawed; SetHeapSize below decommits it.
- A region written moments after its freeze is one the game keeps writing, held thawed for a while ("Hot regions" in
  `runtime/memory.c`).
- A region with reserved holes freezes its committed pages only, and a write thaws them without committing a hole.
- `NX2SARIA_WB_PROBE=1` ("Probed regions" in `runtime/memory.c`): a probed region is inaccessible until its first
  access, which is noted and runs again; a frozen one goes back to read-only first and then thaws on a write (the
  probe's write to `q[0x3000]` ends the probe, read-only again, faults once more and thaws); `mem_thaw` ends a probe
  and holds the region. With the setting off `mem_probe` arms nothing.

## `runtime/gfx/gfx_window_probe.c`

`--probe-window`: presents 300 frames of a cycling clear colour through `vk_present_image`, reads the swapchain image
back and compares every pixel, measures the vsync rate, flips the focus state and drives the cursor release with
synthetic window events (the window back end's `wsys_probe_*` of `runtime/gfx/gfx_wsys.h`) and resizes the window. Run
it with `NX2SARIA_VK_VALIDATION=1` to count validation errors.

### `source_clear()`

Clears the source to `rgba` and leaves it in TRANSFER_SRC_OPTIMAL; the GPU work is waited for here, which is stricter
than the `vk_present_image` contract needs.

### `present_and_check()`

Presents one colour and checks every pixel of the readback; 0 when it matches. `black` (0xff000000) is opaque black in
both byte orders. `r` is the picture's rectangle (`gfx_fit_rect`): bars of black lie around it when the window's shape
differs.

### `probe_window()`

- `vk_hdr_off()` first: the checks read 8-bit swapchain pixels.
- Focus: a synthetic focus loss and gain, delivered synchronously on the window thread.
- Cursor release: Esc releases, a client click recaptures, a title bar activation releases (where the window system
  reports one). The probe window may not be the foreground window, so this checks the release flag, not the capture.
  Esc opened the menu, so it is closed again: the presents below compare the bare game image.
- Resize: the client area changes, the swapchain follows, presenting still reads back exact.
- Vsync: ticks are counted between two tick edges about 5 s apart.

## `runtime/gfx/gfx_shader_probe.c`

`--probe-shader`: the shader service client (`runtime/gfx/gfx_shader.c`) against 32 census shaders. It needs
`build/xlate/nx2navi_xlate.exe` and a census, `build/<version>/shaders/` (`tools/shader_census.py`,
`probe_census_dir()`); it uses a throwaway cache directory.

1. Translate 32 shaders (mixed stages) through `gfx_shader_get`; each SPIR-V starts with the magic 0x07230203.
2. Drop the memory cache and ask again: 32 hits from disk, 0 service requests. A third pass returns the same pointers.
3. Send a malformed line straight to the service: it answers with an error line and still serves a valid request.

### `str_field()`

The value of `"name": "` inside [from, to), NULL when absent; `*len` is the value's length.

### `probe_census_dir()`

The shader census the probes translate (`tools/shader_census.py --version <ver>`): `build/<ver>/shaders` in the nearest
parent of `nx2saria.exe` that has one with a `jobs.json`, the running game version's first, then the others' (any
census serves a probe). 1 with that folder in `dir`. The three names it tries are `v110`, `v121` and `v143`.

### `pick_entries()`

11 vertex, 11 fragment and 10 compute shaders, spread evenly over each stage's list.

### `translate()`

Builds the program the game would hand NVN for a census entry and translates it. `root` is the census folder.

### `probe_shader()`

Pass 1 translates through the service. Pass 2 runs with the memory cache dropped, so every answer comes from the disk
cache. Pass 3: memory hits return the same pointer. Last, the service survives a malformed line and still translates a
valid one.

## `runtime/gfx/gfx_shader_guess.c`

Bind-state answers for a prepared default variant, read from its first translation (`prep_worker` in `gfx_shader.c`,
for seed records `tools/prepare_shaders.py` flags as default variants). A default variant guesses the state play binds:
every texture a 2D one, float vertex inputs and colour outputs. Against the 3,093 (program, answers) pairs a play
session of Ocarina recorded, that guess matched 89.5%: vertex 77.3%, compute 88.1%, fragment 99.6%. With two readings
of the first translation, and one more translation for the programs they change, `--prepare-shaders` matched 98.3% of
them (vertex 97.1%, compute 96.0%, fragment 99.6%; last-frame vertex variants, `runtime/vk/vk_mvprev.c`, are not
prepared):

- **Vertex inputs.** A vertex attribute without an answer is declared float, and a shader that reads it as an integer
  then bitcasts the loaded value to an integer type. Such inputs are answered uint: of the 313 integer inputs met in
  play, all were uint and all were bitcast, against 36 of 3,339 float ones.
- **Compute textures and images.** Translated without texture answers (the seed gives a compute default variant none),
  the translator declares each texture and image with the type its instructions name; those types become the answers,
  image formats included, and a slot it queries without declaring one keeps the 2D default. Compute misses dropped from
  43 to 13. A compute shader play meets untranslated is translated and its pipeline built on the replay thread
  (`runtime/vk/vk_exec_draw.c`, the dispatch's `shader_for` call), so each miss is a stall; the prepare step's driver
  builds of compute pipelines averaged 0.96 s each, 16 at a time, on an RTX 5090.

### `int_read_inputs()`

The Input Locations (a bit mask) whose loaded value some OpBitcast turns into an integer scalar or vector. Per id it
keeps the input Location the id reads (`loc`, -1 none) and the Location decorations (`dec`). The walk starts at word
5; declarations come before the code that uses them. An OpLoad's result takes the Location of its pointer but is a
loaded value, not a pointer, so access chains do not continue from it. The opcodes read: OpTypeInt 21, OpTypeVector 23,
OpVariable 59, OpLoad 61, OpAccessChain 65,
OpInBoundsAccessChain 66, OpDecorate 71, OpBitcast 124; the decoration Location is 30 and the storage class Input 1.

### `answer_type()`

The translator's SamplerType (`GfxShaderRes.type`) as a `GfxShaderAnswers` `tex_type` / `img_type` code; 0 unknown.

### `gfx_shader_guess_refine()`

A slot the translation queries without declaring a texture there (a bindless one) keeps the 2D default.

## `runtime/gfx/gfx_shader.c`

The shader service client. It turns an NVN program (the control section, the code, the c1 constants and the
translator's answers) into SPIR-V plus reflection through `nx2navi_xlate.exe --serve` children, started lazily with
pipes and kept for the life of the process (a pool: `NX2SARIA_XLATE_PROCS` at run time, more for the boot prewarm).
Results are cached in memory and on disk (`shader-cache/<key>.spv` and `<key>.res.json` next to `nx2saria.exe`), so a
shader is translated once per machine and translator. Why the shaders are prepared on the player's PC and not shipped:
[shader-delivery-research.md](../shader-delivery-research.md).

**Keys.** The code hash is sha256 over the stage (u32), the 0x880-byte control section, `code_size` (u64), the code,
the c1 length (u32) and its bytes. Per code hash and translator the cache holds a use mask
(`<sha256("TKSU", xlate id, code hash)>.use`): the texture and image slots the translator queried (`tex_queried` and
`img_queried` in the `.res.json`), the vertex attributes a vertex shader declares and the colour targets a fragment
shader declares (SPIR-V Locations). The input id is sha256 over "TKSI", `IN_VERSION`, the code hash and the answers
with everything outside the mask zeroed (a fixed little-endian layout), so stale bindings in slots the shader never
reads do not make new ids; it does not depend on the translator. The cache key is sha256 over "TKSK", `KEY_VERSION`,
the translator identity and the input id. A code hash without a known mask is translated under a provisional key
("TKSP", the full answers); its files are renamed to the cache key and the mask stored.

**Translator identity.** `build/xlate/nx2navi_xlate.id` holds the sha256, in hex, that `tools/build_xlate.py` writes
over the sources it builds from (`Program.cs`, the csproj, the patches, the vendor tree); without that file the
identity is the sha256 of the exe. `NX2SARIA_XLATE_ID` (64 hex digits) overrides it. A translator change therefore
changes every cache key without a hand-made `KEY_VERSION` bump; `KEY_VERSION` is for changes on this side (the key
layout, how the output is used). Nothing in this file or its folder is part of that identity.

**Carry** (`NX2SARIA_SHADER_CARRY`, default 1). Since every cache key names the translator, a release with a new
translator would otherwise translate every shader again, before play or while the game waits.
`<cache>/identities.txt` lists the translator identities the cache holds translations of, newest first: base
identities, before `gfx_shader_set_host()` mixes the host options in. The current one goes first once a translation
under it is stored or read (`ids_note()`). A shader the current translator has no translation of takes a previous
translator's: its use mask, input id and cache key are computed with that identity mixed with this machine's host
options, so a translation made with other host options never matches, and the `.spv` and `.res.json` come from one
place (the pack or the loose files) and pass the same checks as any entry. Lookups go: the current key in memory, the
previous keys in memory, the current key on disk, the previous keys on disk, then a translation (`carry_find()`). A
shader served from a previous translator thus keeps that translation for the rest of the session, deliberately: no
pipeline is built a second time for it, and the pass below needs no way to swap shaders under the renderer. A shader
first met after the pass translated it gets the new translation. A translation that indexes the texture pool with the
earlier translator's length is not served (`carry_usable()`): its clamp would draw the textures numbered past that
length with another. It serves only once the current translator has failed the shader (`carry_last()`), as the last
resort.

The background pass (`gfx_shader_carry_start()`, after the boot prewarm) translates every stored input that has a
previous translator's entry again under the current identity, on a translator child of its own that runs at idle
priority, outside the pool, and waits while on-demand translations are queued or running. It writes cache files only;
nothing enters the memory cache. A program the new translator refuses keeps its previous translation, and the log names
it. When the pass has gone through every such input it drops the previous translators' entries of the inputs it
covered: loose files are deleted, packed ones are left out of a pruned copy of the pack, which replaces the pack at the
next start if the pack is still the file it was copied from. The stored inputs (`in/`) are never dropped. An identity
leaves `identities.txt` when nothing of it is left. Only the process holding `<cache>/carry.lock` runs the pass or
replaces the pack. The preparation (`prepare_files()`, `--prepare-shaders`) does not carry: it translates everything
under the current identity.

One limit: `runtime/vk/vk_exec_draw.c` learns a program's texture slot mask from the first shader it gets for it
(`slot_mask_learn`), so a carried shader teaches it the previous translator's mask. Should the current translator read
the type of a slot the previous one did not, a variant of that program translated anew in the same session is asked
without that slot's type (the translator's default). The pass translates from the stored full answers, so the next
session is not affected.

**Pack.** `tools/pack_shader_cache.py` folds the `.spv`, `.res.json`, `.use` and `in/*.in` files into
`<cache>/shader-cache.pack` (one file instead of four per shader: about 117,000 files after an install) and deletes
them; the installer runs it after preparing the shaders. The pack is read-only here. Every read looks in the pack
first and then for the loose file, and new translations are written as loose files as before, until the tool runs
again.

**Stored inputs.** Every translation also writes `<cache>/in/<input id>.in` (magic "TKSI"; the stage, the full
answers, the control section, the code up to its length, c1), so a pipeline key record (`runtime/vk/vk_pipeline.c`),
which names its shaders by input id, can be translated again under a new translator without guest memory
(`gfx_shader_load_input()`, `gfx_shader_prewarm()`).

**Durability.** Every file appears whole: it is written under another name and renamed. Only the `.in` files are also
flushed to the disk before their rename (`plat_file_write_atomic`): they hold the guest's code, which nothing else can
give back once the game is not running, and `in_load()` checks only their header and size, so a power cut that tore one
would have a wrong shader translated from it. The `.spv`, `.res.json` and `.use` files are not flushed
(`plat_file_write_cached`, `plat_file_replace` without durable): every read checks them (`spirv_complete()`,
`reflection_valid()`, the `.use` magic and size) and a torn one is translated again. A flush waits for the drive, 1-5
ms on an NVMe drive and far longer on a SATA SSD or a hard drive, and the preparation of an install translates about
36,000 shaders: flushing these files too held its translation at half speed on an NVMe drive.

**Reflection.** Every shader that enters the cache has its `.res.json` parsed into `GfxShaderRes` arrays and its
SPIR-V scanned for interface Locations, the workgroup size and its Binding decorations (the accessors of
`runtime/gfx/gfx.h` at the end of the file).

**SPIR-V at rest** (`NX2SARIA_SHADER_COMPRESS`, default 1): the memory cache keeps every shader for the life of the
process, but its SPIR-V is read only while modules are made of it. A background thread keeps the SPIR-V of a shader
nobody read for `NX2SARIA_SHADER_COMPRESS_IDLE_S` seconds (fewer while the system memory is tight, `mem_budget.h`) as a
zstd frame alone, and a reader that pins it (`gfx_shader_spirv_pin()`) gets it decoded back
(`runtime/gfx/spv_store.h`). The numbers are under `zpass_main()` below.

The c2 handle ranges of `handle_slot()` are the translator's handle layout. The code layout the translator sees
(graphics: `code[0x30..]`, compute: `code[0..]`; c1 = `code[c1..length]` with `c1` and `length` at control+0x700)
follows `tools/shader_census.py`.

### Constants, types and state

- `CODE_SKIP` (0x30): graphics code starts after the first 0x30 bytes of the program.
- `KEY_VERSION` (5) is in every cache key; bump it when the key layout or the use of the output changes here (4: the
  translator identity and input ids; 5: the NVN2 compute shared memory size in the request). Translator changes are
  covered by its identity.
- `IN_VERSION` (1) is in every input id and `.in` file: bump it when the answers layout or the program hash changes
  (that drops every stored input).
- `USE_MAGIC` 0x4d554b54 is "TKUM", the `.use` files; `IN_MAGIC` 0x49534b54 is "TKSI", the `.in` files. `MAX_SVC` (32)
  is the number of translator processes. `PATH_CAP` (1024) is the bytes of a UTF-8 path.
- `UseMask`: which answers a shader's translation depends on: `slots` (`tex_type`, `img_type`, `img_format` of these
  c2 slots), `attrs` (`attr_type`, the vertex input Locations) and `fouts` (`frag_out_type`, the fragment output
  Locations).
- `UseEnt` maps a code hash to its use mask; `known` 0 caches a disk miss. `use_buckets` is under `cache_lock`.
- `struct GfxShader`: `iid` is the input id. `spv` is the SPIR-V: plain while the shader is built (`spv.plain`,
  `spv.words`), then pinned by readers and compressed when idle; `spv_lock` guards what `spv_store.h` says. `bind`
  holds its OpDecorate Binding values in module order (`spirv_seal()`). `reflection` is the service's `.res.json`
  text, parsed into the fields after it by `reflect()`, once per shader before it enters the cache: `stage`
  (`GFX_STAGE_*`), `in_mask` and `out_mask` (the user Locations of Input and Output variables), `mvprev_banks` (the
  `.res.json` "mvprev_banks" of patch 0015, 0 none), the matrix palette of a vertex shader (`reflect_palette()`: the
  bytes of one matrix, 0 none, the uniform bank or storage buffer index it is read from, and the input Locations whose
  integers pick the matrices), `omap` (fragment: the `.res.json` "frag_out_map", the colour components it writes, 4
  bits per target, the shader header's omap; 0 unknown, as in older translations), `carried` (1 + the previous
  identity, `C.prev`, that translated it; 0 the current translator) and `fresh` (translated in this process,
  `translate_one()`).
- `cache_lock` guards the memory cache; `spv_lock` every shader's `SpvStore`. `n_keys` counts memory cache entries.
- `Svc` is one translator child: `child` NULL means not started; `busy`, a request owns it (under `S.lock`).
  `S.limit` is the children in use at most (0: `svc_runtime_limit()`).
- `F` holds the translations in flight: by cache key, or by code hash while the use mask is unknown; one translation
  per id at a time.
- `xlate_id` is the translator identity; `xlate_base` the same before `gfx_shader_set_host()` mixes in the host
  options, whose values are `host_f64`, `host_zero_locals`, `host_bda`, `host_sbuf`, `host_sg` and `host_gather`.
- `C` is the carry state; `prev` and `prev_eff` are written before `ready` is set and never change after it.
  `IDS_MAX` (8): `identities.txt` holds at most this many identities. `on` is `NX2SARIA_SHADER_CARRY`, read in
  `init_paths()`; `n_prev` the previous identities in `identities.txt`, newest first; `prev` their base identities;
  `prev_eff` those mixed with this machine's host options (their cache keys); `ready`, `prev_eff` is set
  (`gfx_shader_set_host()`); `suspended`, a preparation runs and everything is translated anew; `noted`,
  `identities.txt` names the current identity first, or will; `dropped`, the identities the pass found nothing of
  (under `ids_lock`); `served`, the shaders served from a previous translator this session; `refused`, the previous
  translations not used (`carry_usable()`). `ids_lock` guards the rewrites of `identities.txt` and `C.dropped`.

### `pack_open()` `pack_find()` `pack_read()`

The cache pack, `<cache>/shader-cache.pack`, written by `tools/pack_shader_cache.py`. Little-endian: "TKSPACK1", a
u32 count, a u32 0, then count entries sorted by (kind, name), then the data. `name` is the 32 bytes the loose file's
64 hex digits spell. The index is read once (48 bytes a file: about 5.6 MB after an install); the data is read per
lookup with positioned reads on one handle, safe from every thread. A pack that fails a check is ignored as a whole and
the loose files are used.

`pack_open()` opens the pack and reads its index (from `init_paths()`); nothing happens when there is none.
`pack_read()` returns a packed file as `plat_file_read` returns a loose one: malloc'd, a NUL after the last byte; NULL
when absent or empty.

### `json_esc()`

Appends a UTF-8 string as a JSON string body: ASCII as it is, quotes and backslashes escaped, the rest as `\uXXXX`
(UTF-16 units), so the service's stdin encoding does not matter.

### `hex32()` `xlate_identity()`

`hex32()` reads 64 hex digits, then nothing but white space. `xlate_identity()` takes `NX2SARIA_XLATE_ID` (64 hex
digits, or any other text, which is hashed), else `<translator>.id` next to the exe (`tools/build_xlate.py`), else the
sha256 of the exe.

### `init_paths()`

The repository root is the nearest parent of `nx2saria.exe` that holds `build/xlate/nx2navi_xlate.exe`. The shader
cache lives next to `nx2saria.exe`, so every build folder has its own (as the pipeline cache,
`runtime/vk/vk_pipeline.c` `cache_path_init()`). A packaged folder (`tools/package_build.py`) keeps the translator next
to `nx2saria.exe`. With the carry on, `ids_load()` and `pack_swap_pruned()` run before the pack is opened (the swap has
to come first).

### `host_text()` `host_mix()` `gfx_shader_set_host()`

`host_text()` writes the host options as they are mixed into an identity into `t` (96 bytes) and returns their length,
0 for the defaults, which are not mixed in. `host_mix()` gives a base identity with this machine's host options mixed
in: the identity its cache keys use here.

`gfx_shader_set_host()` receives the host answers (`runtime/vk/vk_caps.c`, at device creation, before any
translation). Answers other than the defaults (Float64, exact storage offsets, 32 lanes, no gather bias, undefined
locals) are mixed into the translator identity, so they get their own cache keys. The previous identities of
`identities.txt` get the same options mixed in (carry).

### `ids_parse()` `ids_load()` `ids_write()` `ids_note()`

`identities.txt`: "#" comment lines, then one base identity per line in 64 hex digits, newest first, at most `IDS_MAX`.

- `ids_parse()`: the identities of the text `f` (may be NULL) into `out`; it returns their count. Lines that are no
  identity are skipped, and a repeated line counts once.
- `ids_load()` (from `init_paths()`): the previous identities, and whether the current one is first already
  (`C.noted`: then there is nothing to write).
- `ids_write()` rewrites `identities.txt` from what it holds now: the current identity first when a translation under
  it was stored or read (`C.noted`), then the others in their order, without the ones the pass dropped. The file is
  read again first, so an identity another process wrote meanwhile stays.
- `ids_note()`: a translation under the current identity was stored or read: once per process, `identities.txt` gets
  the current identity first (on a thread of its own: the caller may be the render thread, and the write waits for the
  disk).

### `child_exchange_raw()` `child_exchange()`

One line out, one line in (no newline in the reply). 1 answered; 0 when the child could not start or the pipe broke;
-1 when the child sent nothing for `PLAT_CHILD_READ_MS` (stuck on this request: the translator is deterministic, so
sending it again would wait as long). An overlong line is dropped and its end waited for. `child_exchange()` wraps it
in the `NX2SARIA_TRACE=perf` counter of the render thread's time spent translating.

### `svc_runtime_limit()` `svc_used()` `svc_get()` `svc_put()` `svc_set_limit()`

- `svc_runtime_limit()`: the children allowed at run time, `NX2SARIA_XLATE_PROCS`, default 2.
- `svc_used()`: the caller holds `S.lock`.
- `svc_get()`: a child for one request: an idle running one, else a new slot while fewer than `S.limit` are in use,
  else it waits.
- `svc_put()` returns a child; one over the limit (`svc_set_limit()` lowered it) is stopped.
- `svc_set_limit()` sets the number of children; idle ones over it are stopped now, busy ones when they are returned.

### `svc_request()`

One request on a child; a dead child is restarted once. 1 answered, 0 when the translator is unavailable, -1 when the
child gave no answer for `PLAT_CHILD_READ_MS`: it is stopped and the request is not sent again, which would hold the
caller (the replay thread for a compute shader) as long once more. The caller has given up its virtual core
(`kern_core_away`): the wait for a child and the translation are host time.

### `flight_enter()`

One translation per id at a time (`F`); a full `F.id` also waits.

### `carry_served()` `mem_insert()`

`carry_served()`: a shader of a previous translator entered the memory cache: counted, and the first one logged.
`mem_insert()` inserts `s`, or frees it and returns the entry another thread inserted first.

### `reflection_valid()`

A `.res.json` the translator wrote: one JSON object that names its stage, no NUL inside. A file an interrupted write
left zero-filled (3647 of 21924 in one cache were) fails this; `reflect()` would take it for a vertex shader with no
resources, and its pipeline layout would lack the shader's buffers and textures. "ts" and "ms" are NVN2's task and mesh
stages.

### `spirv_complete()`

Every instruction after the header has a word count and the last one ends at the module's end: a module with
zero-filled clusters fails.

### `spirv_seal()`

The last step of making a shader, once its in-memory SPIR-V is final (after `reflect()` and the HDR swap): the module's
Binding decorations are listed (opcode 71, OpDecorate, with decoration 33, Binding; read through
`gfx_shader_bindings()`, which `module_binding_missing` in `runtime/vk/vk_pipeline.c` and `mfb_binding_base` in
`runtime/vk/vk_exec_draw.c` read in place of the SPIR-V: the first runs on the drawing thread at every pipeline miss,
and must not decode a compressed module there), and the store gets its first use stamp, so that a shader just loaded is
not compressed before its first pipelines are made.

### `disk_load_from()` `disk_load()` `entry_present()`

`disk_load_from()` reads `<key>.spv` and `<key>.res.json` from the pack (`packed` 1) or the loose files; NULL if either
is missing or malformed. `iid` is the input id, NULL when the caller sets it. With `full` 0 only the stage and the
interface Locations are reflected and the SPIR-V is left as the translator wrote it (the carry pass checks a
translation it does not keep). A bad entry's log line says what was wrong (the first 3 and then each power of two): an
unknown stage name has cost a session before. For a fragment shader `vk_hdr_shader_swap()` changes the in-memory copy;
the cache keeps its own.

`disk_load()` tries the pack, then the loose files (a newer translation of a key the pack holds a bad copy of); NULL
when neither has a valid pair (the caller translates again and writes loose files). `entry_present()` says whether the
cache holds `<key>.spv` and `<key>.res.json` together in the pack or together as loose files (as `disk_load()` reads
them), without reading them.

### Request building and the reflection's JSON

`tex_names` are the `GfxShaderAnswers` `tex_type` / `img_type` codes; `fmt_names` the `img_format` codes, in the order
of the translator's TextureFormat enum. The reflection reads the `.res.json` the translator writes (`Translate` in
`tools/xlate/Program.cs`) and the SPIR-V itself.

- `json_key()`: `"name":` inside [o, e): the first byte after the colon and its spaces, NULL when absent.
- `json_str()` copies a string value into `buf` (empty when absent or not a string).
- `json_objects()`: the objects of the flat array `"name": [ {...}, ... ]`: it calls `fn` for each object's [start,
  end) and returns the count.
- `handle_slot()`: the c2 slot of a texture or image handle (the translator's layout: combined handles at 0x20, 32
  slots; images at 0x120; separate textures at 0x168), -1 outside those ranges or for a handle in another constant
  buffer.

### `reflect_spirv()`

The interface Locations and the compute workgroup size from the module. The opcodes and enumerants are those of the
SPIR-V 1.6 specification, section 3: OpEntryPoint 15, OpExecutionMode 16, OpTypeInt 21, OpTypeFloat 22, OpTypeVector
23, OpTypeArray 28, OpTypePointer 32, OpConstant 43, OpVariable 59, OpDecorate 71; the decorations BuiltIn 11, Location
30, Component 31; the storage classes Input 1 and Output 3; the execution mode LocalSize 17. Ids beyond 2^20 are
ignored (translated modules stay far below).

Per id it keeps the location + 1 (0: none), the component flag, the scalar or vector type and the array span, the
pointer target and the constant value. The interface masks cover 32 locations. Arrays passed from a vertex to a
fragment shader occupy every element's locations, even if only some elements are accessed. Other stages retain their
existing reflection: mesh outputs have an outer per-vertex array.

### `patch_txd_transpose()`

Every gradient sample (OpImageSampleExplicitLod, 88, with the Grad operand, 4) whose two gradients are each built from
two components gets them transposed, dx = (a, c) and dy = (b, d) from dx = (a, b) and dy = (c, d), by two new
OpCompositeConstruct (80) before it. The game's shaders fill a TXD's gradient registers per coordinate (du/dx, du/dy,
dv/dx, dv/dy), and the translator builds dx = (the first two registers) and dy = (the last two), that is (du/dx,
du/dy) and (dv/dx, dv/dy): a mip level chosen from the wrong derivatives, too sharp on terrain faces turned across the
screen, which crawled in patches that followed the faces. `NX2SARIA_TXD_TRANSPOSE=0` turns it off.

It is applied to the SPIR-V after it is loaded, so the shader cache is untouched. Not for a Switch 2 title (NVN 200 and
up): its SM86 programs are lowered by `tools/xlate/AmpereLowering.cs` (Txd), which puts SM86's interleaved derivatives
(dx.x, dy.x, dx.y, dy.y) in the translator's order already, so transposing again here swapped them back and chose
Ocarina's mip levels from the wrong derivatives. `NX2SARIA_TXD_TRANSPOSE=1` forces it on.

`cc` holds, per id, the word index of a 2-constituent OpCompositeConstruct defining it; the two it reads are [header,
type, id, a, b] and [header, type, id, c, d].

### `reflect_exact()`

Texture detail (`runtime/vk/vk_texture.c`) makes large material textures without their top mip levels, which a shader
that samples with normalised coordinates cannot tell. One that reads texels by position can, and so can one that works
with the size or the level count. This finds the sampled textures a module reads so and sets `GfxShaderRes.exact` on
their bindings; the replay then binds such a texture whole (`tex_info_full` in `runtime/vk/vk_exec_draw.c`):

- OpImageFetch;
- OpImageQuerySize and OpImageQueryLevels;
- OpImageQuerySizeLod, unless the image is arrayed and every use of the result is an OpCompositeExtract of component
  2, the layer count: the translator clamps the layer of an array sample to it, and no level changes it.

The image of each read is followed back to its variable through the loads, access chains, OpSampledImage and OpImage
the translator emits. A read that leads to no variable marks every texture of the module. One that leads into the
texture pool array (a bindless handle picks the texture, so no binding names it) cannot be acted on and is logged: none
of the 44,081 shaders prepared for game 1.1.1 has one.

Nine shaders in ten have no such read: they cost only the first walk over the instruction headers. Per id the function
keeps: a type's or variable's image is arrayed; a variable's Binding + 1; the variable a value comes from; the variable
a pending size query read (+ 1); and per variable whether it is read exactly. Then every use of a pending size query
is looked at: anything but the layer count makes its texture exact. Any word that equals the result's id counts as a
use, so a literal that happens to equal it errs towards exact.

`gfx_shader_exact_seen()` returns the count this function keeps (`n_exact_shaders`).

### `patch_vec_index()`

Every load of one component of a uniform block vector at a computed component index (an OpAccessChain into a Uniform
block whose last index, into a vector, is not a constant: the translator's `c[n][i >> 2][i & 3]` for an LDC with a
register offset) becomes a load of the whole vector, its components extracted with constant indices and the one wanted
picked with OpSelect. AMD's Windows drivers can read the wrong component through such an access chain, the bug the
translator's own source names HasVectorIndexingBug (VectorComponentSelect). The volumetric fog's per-volume extinction
reads its colour this way. It did not change the fog's teal blotches on an RX 580 (driver 23.10.01.14), so it is off by
default. `NX2SARIA_VEC_INDEX_FIX`: 1 everywhere, `auto` on AMD's own drivers (`vk_caps_vec_index_bug()` of
`runtime/vk/vk_caps.c`), unset or 0 off. In memory only.

The opcodes it uses: OpTypeBool 20, OpTypeInt 21, OpTypeFloat 22, OpTypeVector 23, OpTypeArray 28, OpTypeRuntimeArray
29, OpTypeStruct 30, OpTypePointer 32, OpConstant 43, OpFunction 54, OpVariable 59, OpLoad 61, OpAccessChain 65,
OpInBoundsAccessChain 66, OpCompositeExtract 81, OpSelect 169, OpIEqual 170; the storage class Uniform is 2.

- `def` is the word index of the instruction defining each id; `ptr_v`, per vector type, its Uniform pointer type, if
  any; `target`, per access chain id to rewrite, the vector access chain's id; `acv`, per access chain id to rewrite,
  the vector type.
- Finding each instruction's result id: the opcodes 3, 4, 5, 6, 8, 10, 14, 15, 16, 17, 56, 62, 63, 71, 72, 73, 74, 75,
  99, 224, 225, 228, 246, 247, 248 and the others of that case list have no result id, or none at word 2 (debug,
  annotations, mode setting, stores, barriers, control flow); 7 and 11 (OpString, OpExtInstImport) have it at word 1,
  as have the type opcodes (the named ones and the other OpType* from 19 to 39); the rest at word 2.
- Then the access chains to rewrite and what they need: `need_ptr` and `need_ptr_v` are the new Uniform pointer types
  (id, vector type, up to 64); `cint` the constants 0..3 per index type (up to 8 types), taken from the module where it
  has them. The variable's pointee is read from its pointer type, and the last index's type decides.
- The loads are counted to size the output: each becomes 1 + 4 + 3 + 3 instructions of up to 5 words.
- The new declarations go last in the global section. Each rewritten chain is the same chain without its last index:
  a pointer to the vector. Each rewritten load replaces the original.

### `reflect_palette()`

The matrix palette of a skinned vertex shader, for the motion vectors of skinned characters
(`runtime/vk/vk_skin_motion.inc`). The game's skinning reads a joint index from an integer vertex input, multiplies it
by the size of one bone matrix (48 bytes, three rows of four floats; 64 for a 4x4) and reads the matrix at that byte
offset of a uniform block. The translation keeps that shape, so the rule is:

| Term | Rule |
|---|---|
| joint value | a value loaded from an Input variable of an integer type that has a Location, and what bit casts, integer conversions, component extracts and stores to and loads from local variables make of it |
| matrix offset | a joint value multiplied by the constant 48 or 64 (or shifted left by 6), and what integer adds, subtracts, shifts right and bit masks make of it (the translator turns the byte offset into a vec4 index and a component), again through local variables |
| palette | the uniform or storage block one of whose access chains takes a matrix offset as an index |

Local variables are followed whatever the order of their stores and loads. A shader whose matrix offsets reach two
different blocks has no palette (none does). Counted over the 1,460 vertex shaders of the 6,152 pipelines a played
folder recorded (game 1.1.1): 254 are drawn with an unsigned integer attribute in their vertex layout, and the rule
finds a palette in 253 of them (the last reads its integer attribute as a float and indexes nothing with it) and in
none of the other 1,206. Every palette is a uniform block of 48-byte matrices, at c6 (180 shaders) or at c7 (73), so
the bank cannot be told from the list of banks a shader reads. 208 of the 253 layouts also carry weights (a normalised
attribute beside the indices); the other 45 bind each vertex to one joint and carry none.

Most vertex shaders read no integer input and stop at the first check. Per id the function keeps: an integer type; a
pointer type's target; an integer constant 6, 48 or 64, as its value + 1; a variable's Location + 1 and Binding + 1; a
block variable; the Location + 1 a joint value came from; the matrix size of a matrix offset. Binding 0 is the support
buffer and is no block. The body is walked again while a pass learned something, 4 passes at most: a load may stand
before the store it reads (a loop).

The opcodes it uses: OpFunction 54, OpLoad 61, OpStore 62, OpAccessChain 65, OpInBoundsAccessChain 66,
OpCompositeExtract 81, OpCopyObject 83, OpUConvert 113, OpSConvert 114, OpBitcast 124, OpIAdd 128, OpISub 130, OpIMul
132, OpShiftRightLogical 194, OpShiftRightArithmetic 195, OpShiftLeftLogical 196, OpBitwiseOr 197, OpBitwiseAnd 199;
the decoration Binding is 33, the storage classes Uniform 2 and StorageBuffer 12.

### Use masks

- `json_slot_mask()`: the slots of the int array `"name": [...]` below 32, as a mask.
- `use_name_id()`: `<sha256("TKSU", translator identity, code hash)>.use`: masks are per translator.
- `UseFile` and `use_preload()`: every `.use` file of the cache is read by a background thread when the paths are set,
  by file name: a lookup on the render thread then opens no file. Opening one took 0.1 ms alone but 11-40 ms in play,
  behind the translator's and the pipeline cache's writes and flushes on the same volume, and each new shader of a draw
  opened one. The table (`USE_FILE_BUCKETS`, 65536 buckets) is complete and read-only once `use_files_ready` is 1;
  masks found later are in `use_buckets`. `use_files_n` counts the entries `use_preload()` made (`gfx_shader_ram()`).
  It reads the packed masks first; the loose files after them add the masks found since.
- `use_preload_file()`: one `<64 hex digits>.use` file of the cache directory into the table.
- `use_slot()`: the caller holds `cache_lock`. `use_put()` sets or adds the entry for `h`; the caller holds
  `cache_lock` exclusively.
- `use_find()`: 1 with `*m` when the use mask of `h` is known (memory, then `<hash>.use`; a disk miss is remembered).
  Once the preloaded table is complete, a name not in it has no file.
- `cache_flush()`: `NX2SARIA_SHADER_CACHE_FLUSH` (default 0): 1 flushes the `.spv`, `.res.json` and `.use` files to the
  disk too, as the cache did before (see Durability above), for a drive or setup where a torn file was seen.
- `use_store()` does not flush by default: a torn mask fails its magic and size check.

### `pack_answers()` `mask_answers()` `key_of_input_id()` `key_of_input()` `iid_of()` `build_key()`

- `pack_answers()`: the fixed-layout answers for hashing; it returns the byte count (at most 256).
- `mask_answers()`: the answers with everything outside the mask zeroed; the translator ignores `depth_minus_w`
  (`tools/xlate/Program.cs`). `mvprev` is kept: it is another translation, not an answer the use mask covers.
- `key_of_input_id()`: the cache key of an input id under the translator identity `id` (with the host options mixed
  in). `key_of_input()`: the same under the current translator.
- `iid_of()`: the input id of a code hash and answers under use mask `m`.
- `build_key()`: the input id and cache key (`m` given), or the provisional key (`m` NULL: the full answers; `iid`
  untouched).

### `build_request()`

- NVN2 compute programs carry their shared memory size in the control section (+0x64, bytes; nonzero exactly for the
  programs that use shared memory). Without it the translator declares its 4 KiB default, and a program that uses more
  (80 of 2643 in a gameplay dump, up to 18 KiB) stores out of range, which NVIDIA GPUs fault on (device lost at the
  first gameplay frame).
- Their local memory, in bytes a thread, follows at +0x68. Without it the translator declares its 0x1000-byte default:
  one compute program of the game needs 4,752 bytes a thread and indexed past the end of its array.
- The workgroup size of a compute shader comes from the answers, else from control +0x748, or +0x58 for NVN2 (magic
  0x98761234). NVN2 controls carry no workgroup size there; all zero becomes 1, 1, 1.
- Control byte +0x718 of a fragment shader: early fragment tests (the NVN driver's early-Z-force). Without it a shader
  that stores to memory is depth-tested late, and the G-buffer draws' EQUAL test after the depth prepass fails along
  contour lines. It is already part of the code hash (the whole control section).
- The subgroup size index follows the `gfx_shader_set_host()` answers: 0 vertex, 2 compute, task and mesh, 1 the
  rest. `"bda"` is the device address table (`runtime/vk/vk_gpa.c`).
- A control section whose magic is `NVN2_CONTROL_MAGIC` gets `"sm86":true` and its size.
- Task and mesh programs (`MeshInfo` in `tools/xlate/AmpereLowering.cs`): +0x58 the workgroup size (low 16 bits) and
  the payload size in units of 256 bytes (bits 24-31); +0x5c byte 3 the primitive type, byte 2 whether a task stage
  feeds the mesh stage; +0x60 the vertex limit (low 16 bits) and the primitive limit (high 16 bits).
- The compute binding table in the driver bank: the texture entries (byte +0xb3), the image entries (+0xb6), and where
  the table ends (u16 at +0xba); 8 bytes an entry, textures first.

### `use_lookup()`

The use mask of code hash `h` under identity `id`: the preloaded table once it is complete, else the pack and the
loose file. It is not remembered in `use_buckets`, which holds the current identity's masks.

### `pool_short()` `pool_short_note()`

`POOL_TEX_SHORT` (32768) is the texture pool length of the earlier translator (`VK_POOL_TEX_CARRIED` in
`runtime/vk/vk.h`). Its pool arrays clamp every handle to slot 32767, so a texture the game numbers from 32768 up
(Ocarina's pool has 33068 descriptors) is drawn with whatever slot 32767 holds: ground seen through after an area
load. The current translator declares `VK_POOL_TEX` (40960).

`pool_short()`: whether `s` declares a texture pool array (reflection "pool" 1) of the earlier length.
`pool_short_note()`: shaders that entered the memory cache with a texture pool array of the earlier length
(`mem_insert()`), all counted; `vk_texture_register` in `runtime/vk/vk_texture.c` names the count when the game
registers a texture they cannot reach. `carry_usable()` keeps the carried ones out except as the last resort
(`carry_last()`, which logs those), so the first one that is not carried is logged: it comes from a translator in use
that is older than the runtime's pool (a dev build pointed at an old one, or an install whose translator was not
rebuilt).

### `carry_usable()`

Whether previous identity `j`'s translation `s` may serve. One that indexes the texture pool with the earlier length is
not used: the lookup goes on to the next previous identity and then to a translation under the current one (the boot
prewarm translates the shaders it names on the boot screen, others are translated at first use). Served, it would keep
the clamp for the whole session. Only when the current translator fails the shader does it serve after all
(`carry_last()`). Counted for `gfx_shader_record_flush()`; the first logged.

### `carry_load()`

Previous identity `j`'s entry `key` from the disk cache into the memory cache; NULL when the cache has no valid one.
`stage` (`GFX_STAGE_*`, -1 any): an entry of another stage than the request's is not used, nor one `carry_usable()`
refuses unless `short_ok` (`carry_last()`, after the current translator failed the shader). Every entry passes
`disk_load()`'s checks (whole SPIR-V, a `.res.json` naming its stage, both from the pack or both loose), which matters
most for compute shaders: the replay thread dispatches one the moment it has it (`shader_for` in
`runtime/vk/vk_exec_draw.c`).

### `carry_find()` `carry_find_iid()`

`carry_find()`: a previous translator's translation of (code hash, answers) for a stage, newest identity first: from
the memory cache only (`disk` 0), or from the memory cache and then the disk cache (`disk` 1). NULL when none has one or
carry is off. `short_ok` is `carry_load()`'s. `carry_find_iid()` is the same by input id; with `owned` given
(`gfx_shader_load_private()`) an entry read from the disk is handed to the caller with `*owned` 1 instead of entering
the memory cache.

### `carry_last()` `carry_has_iid()`

`carry_last()`: the current translator failed the shader of cache key `key` (code hash `h`): a previous translator's
translation that `carry_usable()` refused serves after all, the last resort. Drawn with the clamp, only the textures
numbered from 32768 up are wrong, where without it every draw of the shader would be skipped, or a compute dispatch
left out, for the whole session. It enters the memory cache, so the lookups' memory step serves it from then on. NULL
when none has one. `carry_has_iid()`: whether the cache holds a previous translator's entry of input `iid`.

### `zpass_main()` `zpass_idle_ms()` `zpass_kick()` `spv_held()`

SPIR-V at rest (`NX2SARIA_SHADER_COMPRESS`, default 1). The memory cache keeps every shader for the life of the
process: 7115 shaders and 777 MiB of the C runtime heap in a session of 2026-11-13, nearly all of it SPIR-V (a
`.res.json` averages 1.8 KB). The SPIR-V is read where modules are made of it: the pipeline workers and the boot's
builds (`create_graphics`, `lib_create`, `emu_get` in `runtime/vk/vk_pipeline.c`), a compute pipeline the drawing
thread creates at a dispatch (`compute_create`), a mesh shader's outputs at its first pipeline key (`mesh_outs`), the
mesh fallback's workers (`mfb_module` in `runtime/vk/vk_exec_draw.c`), an emulated mesh draw's primitive
(`mesh_emu_draw_prim`), the shader preparation (`runtime/gfx/gfx_shader_guess.c`) and the diagnostics
(`runtime/vk/vk_dump.c`, `runtime/gfx/gfx_shader_probe.c`). What the drawing thread compares at every pipeline miss or
draw comes from values kept apart: the module's FNV-1a (`gfx_shader_spirv_fnv()`: `vk_dlaa.c`, `vk_crumbs.c`, the water
corrections, `NX2SARIA_SKIP_SHADERS`) and its Binding decorations (`gfx_shader_bindings()`: `module_binding_missing`),
so those decode nothing.

Every reader pins the SPIR-V (`gfx_shader_spirv_pin()`, `gfx_shader_spirv_unpin()`; `runtime/gfx/spv_store.h` has the
lock rules). One background thread (`zpass_main()`) walks the cache every `ZPASS_PERIOD_MS` (2000) and compresses the
SPIR-V of each shader no reader pinned for `NX2SARIA_SHADER_COMPRESS_IDLE_S` seconds (zstd level `ZPASS_LEVEL`, 1,
checked by decoding it before the plain copy goes), then frees the plain copy. The frame stays with the shader, so a
shader read again keeps its decoded copy until it is idle once more, and then only loses that copy. The thread takes
`spv_lock` for a few loads and stores per shader at normal priority, so that no reader waits for the scheduler to run a
lowered thread inside the lock, and encodes at the lowest priority with no lock held.

Measured on the shader cache of a test install (44,151 modules, 3,835 MiB, mean 91 KB, largest 4.8 MB): level 1 keeps
26.2% of the bytes (3.8 times smaller), encodes at 630 MB/s and decodes at 1.4 GB/s, 60 us for a mean module and about
3.4 ms for the largest compute shader; level 3 kept 26.5% and level 6 25.1% at a third of the speed, and a trained
dictionary saved 1% more. The memory report shows the SPIR-V held, what it would be uncompressed, the shaders held
compressed and the decodes, those on the drawing threads apart (`gfx_shader_ram()`).

- `Z`: `pass` is held over a pass, and `gfx_shader_drop_memory()` takes it before it frees entries; `state` 0 no
  shader yet, 1 compressing, -1 off (`NX2SARIA_SHADER_COMPRESS=0`); `idle_s` is `NX2SARIA_SHADER_COMPRESS_IDLE_S`;
  `frames` and `frame_fail` count the frames made and those the encoder or its check refused; `dec` and `dec_ns` the
  decodes in pins (`gfx_shader_spirv_pin()`, `gfx_shader_spirv_fnv()`); `dec_draw`, `dec_draw_ns` and
  `dec_draw_max_ns` those on the drawing threads (`prof.h`).
- `spv_held()`: the bytes of a shader's plain SPIR-V and of its frame, as the memory report counts them (no lock:
  `spv_store.h` writes both pointers and the frame's size atomically).
- `zpass_idle_ms()`: how long a shader goes unpinned before its SPIR-V is compressed:
  `NX2SARIA_SHADER_COMPRESS_IDLE_S`, at most 5 s while the system memory is tight and 1 s while it is critical
  (`mem_budget.h`). Every level compresses losslessly; a shorter wait only means that a shader read again soon after is
  decoded once more.
- In `zpass_main()`, entries are inserted at a bucket's head and never unlinked while `Z.pass` is held: the chain read
  from the head stays valid without the lock, and an entry inserted meanwhile waits for the next pass.
- `zpass_kick()`: the first shader that enters the memory cache starts the background thread.

### `gfx_shader_raw_request()` `gfx_shader_requests()` `gfx_shader_cache_hits()` `gfx_shader_drop_memory()`

- `gfx_shader_raw_request()`: a raw line exchange for the probe: one line to the service, its reply in `reply`. It
  starts the service if needed.
- `gfx_shader_requests()`: the lines sent to the service. `gfx_shader_cache_hits()`: the gets answered from memory or
  disk.
- `gfx_shader_drop_memory()` drops the memory cache (the probe uses it to prove the disk cache). Pointers handed out
  before are invalid. A pass of the SPIR-V thread ends first: it walks the entries without `cache_lock`
  (`zpass_main()`). The preloaded `.use` table misses the masks this run wrote, which only `use_buckets` held, so the
  lookups go back to the disk.

### `req_init()`

`Req` is a validated request: the program's code length, its c1 constants and the cache key; `req_init()` returns 0
(failure counted) when the program is invalid. `skip` is the bytes of the program before what the translator reads;
`sm86` marks an NVN2 program (lowered to Maxwell by the translator); `key` is the cache key, or the provisional one with
`final` 0; `iid` the input id with `final` 1.

NVN2 (Switch 2) programs: a 0xe0-byte control section (magic 0x98761234, the version at +4, the code size including
the 0x80-byte shader program header at +0x38, the register count at +0x4c) and SM86 code (128-bit Volta+ instructions
after a Turing-style 0x80-byte header; compute programs have no header). The translator lowers them to Maxwell
(`tools/xlate/AmpereLowering.cs`, job "sm86"); the code is sent from its first byte. The control section is the first
0xe0 bytes only (`NVN2_CONTROL_SIZE`): `nvn2_control()` copies them into a zeroed `CONTROL_SIZE` buffer at the entry
points, so the bytes after it (other guest data) stay out of the hashes, stored inputs and jobs. The layout of an NVN2
program: the code (its size at control +0x38), alignment padding, then the c1 constants (jump tables of BRX, compiler
constants) from the offset at +0x40 to the one at +0x44. They are hashed and sent with the code; when those two
offsets do not fit the program, the length is the code size and c1 is empty.

`gfx_shader_sm86_refused()` counts the NVN2 programs the translator refused.

### `failed_slot()`

Keys whose translation failed in this process, so the synchronous path does not send the same program to the
translator on every draw (the background path keeps failed keys in `X.waits`). Open addressing, `FAILED_N` (4096)
slots; when full, failures are no longer remembered.

### `get_one()`

The lookup order with carry: the current key in memory, a previous translator's entry in memory, the current key on
disk, a previous translator's entry on disk. A key whose translation failed skips the last: that lookup found nothing
usable before the translation, and a refused entry (`carry_usable()`) would otherwise be read from the disk again at
every call (a compute shader's at every dispatch, `shader_for` in `runtime/vk/vk_exec_draw.c`). When the translation
fails, a refused entry serves after all (`carry_last()`); it is in memory then, so the key is not marked.

### `in_store()` `in_load()`

The stored inputs, `<cache>/in/<input id>.in`. After the `InHeader` come the `GfxShaderAnswers`, the control section,
`code[length]` and `c1[c1_len]`. `Inputs.buf` is the file; the other pointers point into it.

`in_store()` stores the inputs of a translation unless they are stored already; the file appears whole (written, then
renamed) and is on the disk before its rename, the one cache file that is. `in_load()` reads stored inputs into `*in`
(free `in->buf`); 0 when absent or malformed.

### `prepared_enter()` `prepared_copy()`

Only preparation workers share translator results (`PreparedJob`). Logical shader and input ids continue to include
the full guest program. The child sees the bytes of the code file and the serialized options in `build_request()`.

- `prepared_enter()`: one worker owns a cold request. A failed owner leaves it available for the next worker to retry.
- `prepared_copy()` copies the unmodified disk SPIR-V; `disk_load_from()` applies the per-install HDR substitution in
  memory. The translator embeds its job key in the reflection, so each logical job gets the key it would have emitted.

### `translate_one()` `translate()`

`translate()` is the service round trip for a request that is in neither cache. One translation per cache key (per
code hash while the use mask is unknown) runs at a time; different shaders run in parallel on the children.

In `translate_one()`:

- Another thread may have translated the shader while this one waited in `flight_enter()`, so the caches are looked at
  again first.
- `NX2SARIA_XLATE_KEEP=1`: a last-frame variant's job stays as `<key>.job` (one request line) with its code, to run
  the translator on by hand (`nx2navi_xlate --serve < <key>.job`); 2 keeps every job translated in this run (delete a
  key's `.res.json` and `.spv` from the cache to have it translated, and kept, again).
- A failed translation's job and code are kept for diagnosis, and those of a request the translator never answered.
  Whatever a stopped child wrote is not a translation, so its files are deleted.
- The files are not flushed: the translator wrote them whole, and `disk_load()` checks both.
- With a provisional key the files are named after the cache key and the mask is remembered; when the rename fails the
  mask is kept for this run only.

### Background translation

`gfx_shader_get_async()` runs `svc_runtime_limit()` workers, one per translator child. A job (`XJob`) owns copies of
the control section and the code, so the guest may free or rewrite its program while the job waits; `r.c1` points into
the copied code. A key stays in `X.waits` while its job is queued or running, and for good when the translation failed
and no previous translation serves instead (`carry_last()`), so a failing shader is not sent again on every draw.

- `X`: `done` is signalled when a job finished (`gfx_shader_await()`); `lwork` when a disk read was queued
  (`load_worker()`); `lhead` and `ltail` are the disk reads past the render thread's budget (`load_budget()`);
  `started` is 1 running, -1 no thread could be created; `loaders` the `load_worker()` threads running.
- `n_disk_sync`, `n_disk_sync_ns` and `n_disk_bg` feed `gfx_shader_disk_counts()`.
- `load_budget()`: whether the render thread may read from the disk cache now. Disk reads in `get_async_one()`, the
  render thread's lookup: a shader that is in the disk cache but not in memory yet is read there while those reads stay
  within `LOAD_SHARE` of the wall time (8: a budget of 1/8), at most `LOAD_BURST_NS` (2 ms) at once; past that a
  `load_worker()` (there are `LOADERS`, 3) reads it, and its draws are skipped (or a one-shot draw waits) as for a
  shader being translated. Reading a pair of loose files the first time took 5.6 ms on Windows, against 0.08 ms once
  the system had them: the time is spent opening the files, not reading them. An area that needed 50 shaders not met
  before held one frame for 100 to 500 ms. A packed pair takes a fraction of a millisecond, so with a pack nearly all
  of them are still read in place. `load_tokens` is under `load_lock`.
- `wait_slot()`: the caller holds `X.lock`.
- `xlate_worker()`: when the translation fails, `carry_last()` puts a refused previous translation in memory, where
  `get_async_one()`'s first lookup finds it.
- `load_worker()` reads the shaders `get_async_one()` queued past its budget, in `get_one()`'s disk order: the current
  key, then a previous translator's entry. One the disk cache lacks goes on to the translation queue, its key still
  waiting.
- `start_workers()` (caller holds `X.lock`): the translation workers and the disk readers, started at the first job.
- `to_front()` (caller holds `X.lock`): the job of `key` to the front of its queue, so the worker takes it next
  (`gfx_shader_await()`).
- `get_async_one()` looks in memory in `get_one()`'s order. The disk is read here when the budget allows or no loader
  runs. Another thread may have queued the key between the checks (`raced`). Past the budget a loader reads it first.
  With no worker, or out of memory, everything is done here.
- `gfx_shader_await()`: the wait counts as the render thread's translation time. A queued job goes to the front of
  both queues, so a worker takes it next. A failure was logged by the worker; when the wait ended first it returns
  with `*pending = 1`.

### `gfx_shader_load_input()` `gfx_shader_load_private()`

`gfx_shader_load_input()`, by input id: memory, then the disk cache under the current translator, then (`xlate` 1) a
translation from the stored inputs; a previous translator's entry in between, as `get_one()` takes them. NULL when none
has it; a failed translation is counted as a failure.

`gfx_shader_load_private()` loads from the disk cache only, as a shader the caller owns and releases with
`gfx_shader_release_private()` (the boot library prebuild warms the driver's pipeline cache with it without keeping 40
KB per shader in memory). When the memory cache has it, that entry is returned with `*owned` 0 and must not be
released. NULL when not cached.

### `gfx_shader_prewarm()` `warm_worker()` `other_inputs()` `in_collect()`

- `Warm`: `named` are loaded into memory, results in `out`; `other` are the other stored inputs, translated when not
  cached and not loaded; `other_carried` counts those a previous translator has an entry of, left to the pass (the
  background pass translates them).
- `pw_done`, `pw_total` and `pw_xlated` are the running prewarm's numbers for `gfx_shader_prewarm_progress()`
  (`boot.c`'s wait screen), copied from its `Warm` while it waits for the workers: the `Warm` lives on that function's
  stack. `pw_ms` is the last prewarm's time once it ended (`gfx_shader_prewarm_stats()`), else -1.
- `in_collect()` takes file names of 67 characters: 64 hex digits and ".in".
- `other_inputs()`: the stored inputs not in `named` (sorted): ids parsed from the `in/` file names. The packed inputs
  come first, under the name their loose file would have; `in_store()` writes no loose file for a packed input, so
  there are no duplicates.
- `gfx_shader_prewarm()` refreshes the numbers every 100 ms and writes a log line every 2 s.

### `gfx_shader_spirv_pin()` `gfx_shader_spirv_fnv()`

`gfx_shader_spirv_pin()`: the plain SPIR-V, decoded first when the shader holds it compressed; valid until the matching
unpin. A frame that does not decode (the packer checked it when it made it) or a decode buffer that cannot be had ends
the process, as every other allocation of this file does.

`gfx_shader_spirv_fnv()` is worked out once per shader, by the first caller or by the SPIR-V thread before it
compresses the module, so that a caller on the drawing thread finds it ready for any compressed shader.

### `gfx_shader_write_code()`

The guest program a shader was translated from, as its stored inputs hold it (the whole program: NVN2 code from its
first byte), written to `path` for a disassembler (`NX2SARIA_DUMP`'s `sh_<hash>.code`), and next to it `<path>.job`,
the translator request that translates it (one line for `nx2navi_xlate --serve`; its "out" names the shader cache, and
"glsl_out" or "lowered_out" added to the job write the GLSL or the lowered Maxwell code). 0: no stored inputs.

### `gfx_shader_slot_mask()`

The slots of the use mask: answers outside it select the same translation (`mask_answers()`), so a caller may leave
their texture and image types zero. It parses the reflection: callers keep the result.

### Answers met in play

The translated-shader key includes the answers a draw's state implies, so a translation made without play (the census)
does not match the keys play asks for. Every (program, answers) that a `gfx_shader_get*` call resolves is recorded next
to `pipeline-keys.bin`, so the first-start preparation (here the `NX2SARIA_TEST_PREPARE_SHADERS` hook) translates
exactly the variants play needs, from these files alone:

| File | Layout |
|---|---|
| `shader-answers.bin` | "NX2NSANS", version, record size; records: stage (u32), code hash, input id, the `pack_answers()` bytes of the full answers, zero padding. One record per input id: answers outside the shader's use mask do not change its SPIR-V, are out of its input id and cache key (`mask_answers()`) and so do not make records of their own (a fight run meets 4126 full pairs, 1238 input ids). The full answers are kept so a translator with a wider mask still gets them. Under a new translator the masks, hence the input ids, are new, and play appends records for them. |
| `shader-programs.bin` | "NX2NSPRG", version, 0; records: stage, length, c1 length (u32 each), code hash, control section, `code[length]`, c1. One per code hash; written before the answers that name it. Game content: it stays in the ignored pipeline cache directory. |

A file with another header is rewritten; a torn tail (a killed process) is cut off at open.

- `ANS_PACKED` (147) is `pack_answers()`' size; `ANS_REC` is 4 + 32 + 32 + 147 + 1.
- `Set64` is an open-addressing set, 0 empty; `set_add()` returns 1 if the value is new.
- `RC`: `pairs` and `progs` are the input ids and code hashes seen (in the files or this run); `ans` and `prg` the
  pending bytes; `pairs_new` those added this run.
- `rec_read()` reads a record file's body (after its header) into `*body`; 0 when absent, and a file with another
  header is removed. Only a file of another format or version is deleted. A read that failed (no memory for the body
  when the system commit runs low, a short read) keeps the file: it holds every program and answer met in play.
  `rec_kept` marks a record file `rec_read()` could not read and kept: nothing is appended to it this run.
- `prg_walk()` walks `shader-programs.bin` records and returns the byte length of the whole records; `fn` may be NULL.
- `gfx_shader_record_at()`: with `rec_kept` set nothing is recorded this run: without what the file holds, appends
  would repeat its records after a possibly torn tail.
- `ans_note()`: a get resolved (`r`, `a`) to `s`: it queues the record when its input id is new, and its program when
  new. Cost: a set lookup under a lock per call; gets happen per (program, answers) change in `vk_exec_draw`'s shader
  table, not per draw.
- `rec_io` guards the record files: appends in order, no read during an append.
- `gfx_shader_record_flush()`: with `force` (the saves and the exit) it also logs the carry's served and refused counts
  when they changed. The files are written outside `RC.lock`, which the render thread takes to record a new answer.

### `prepare_files()` `prep_worker()` `gfx_shader_prepare_recorded()`

Preparation from record files: the answers met in play (`NX2SARIA_TEST_PREPARE_SHADERS=1`) or the seed that
`tools/prepare_shaders.py` writes from its shader list (`nx2saria.exe --prepare-shaders`). Both use the record layouts
above; a seed record has a zero input id (the use mask, hence the id, is the translator's to find) and its pad byte
holds the list priority (0 title, 1 warm-up, 2 rest), with `SEED_DEFAULT` (0x80, `DEFAULT_FLAG` in
`tools/prepare_shaders.py`) set on a default variant: its answers are read again from its first translation
(`runtime/gfx/gfx_shader_guess.c`) and the variant it resolves to is the translation with those, when they differ.

`Prep.iid` holds, per answers record, the resolved input id, zero on failure.

`prepare_files()` translates every record of `ans_path` whose program is in `prg_path` on `threads` threads with up to
`procs` translators. `*ans_out` and `*iid_out` (optional; free them): the answers records and the input id each
resolved to (zero: failed). It returns the record count, -1 without the files. It raises `C.suspended`: a preparation
translates everything under the current translator, carrying nothing. Every 2 s it prints a count on stdout for
`tools/make_game.py` (`step_shaders` reads the "prepare-shaders: progress" lines and shows them in the launcher, which
otherwise sat at 0 % for the whole step), and every 10 s one in the log.

### `gfx_shader_prepared_read()` `gfx_shader_prepare_seed()` `file_sha256()`

The prepared variants, `shader-prepared.bin` next to `pipeline-keys.bin`. The layout is that of
`runtime/gfx/prp_file.h`: a head that says what the list was prepared from (the two seed files and the translator),
then one record per variant: stage, priority, 2 zero bytes, input id. `gfx_shader_prepare_seed()` writes them in
priority order, one per input id; `gfx_shader_warm_list()` reads them at boot.

- `file_sha256()`: the sha256 of the whole file; 0 when it cannot be read. A seed past 2 GB is not read by
  `rec_read()` either.
- `gfx_shader_prepared_read()`: the records of the prepared list at `path` (`PRP_REC` bytes each, malloc'd; their count
  in `*n`), NULL without any. `*from` and `*has_from` (both optional): what a version 2 file says it was prepared from.
- `gfx_shader_prepare_seed()`: what this list is prepared from goes into its head: the seed as it is on the disk now,
  and the translator before the host options are mixed in, which is what `nx2navi_xlate.id` holds. The list there
  before is replaced whole at the end; the log says whether it came from this seed. `C.suspended` is raised for the
  whole run: the variants built into the driver's cache after it (`lb_warm_prepared` in `runtime/vk/vk_pipeline.c`)
  are the current translator's, as before carrying existed. Seed records come in priority order: the first id wins.

### `gfx_shader_warm_list()`

The variants the boot prebuild warms, most likely first: every input id play recorded (`shader-answers.bin`, priority
0), then the prepared ones (`shader-prepared.bin`, priority 1 + their list priority). One entry per input id. It reads
the answers under `rec_io`, so no append happens meanwhile.

### The carry's background pass

- `carry_lock()`: `<cache>/carry.lock`, held for the rest of the process: 1 held, 0 another process holds it, -1 it
  cannot be made.
- `PACK_SRC_MAGIC` "TKSPSRC1" starts the pruned pack's trailer: this, then the source pack's `PlatFileId` as five u64
  (`PACK_TRAILER`, 48 bytes).
- `pruned_of()`: whether `<cache>/shader-cache.pack.pruned` exists and was copied from the pack file `src`; `*size` its
  size.
- `pack_swap_pruned()` (`init_paths()`, before the pack is opened): the pruned copy a pass wrote (`carry_repack()`)
  replaces `shader-cache.pack` when the pack is still the file it was copied from. Otherwise something wrote a new pack
  since (`tools/pack_shader_cache.py`, an install), the copy would lose what that one added, and it is deleted; a later
  pass prunes the new pack. When another process uses the cache, a later start does it.
- `CarryItem`: `iid` and `h` are the input id and the code hash once known (`h_known`); `has` bit j, an entry under
  `C.prev_eff[j]`; `state` one of `CI_TODO`, `CI_COVERED`, `CI_FAILED`, `CI_SKIPPED` (skipped: an unreadable stored
  input, or no translator).
- `carry_svc` is the pass's own translator child, outside the pool (`S`).
- `carry_busy()`: whether on-demand translation is running or queued (`F`, `X`): the pass waits until none is.
- `carry_exchange()`: one request on the pass's child, started below every normal process when needed; a dead child is
  restarted once. 1 answered, 0 unavailable, -1 no answer for `PLAT_CHILD_READ_MS`: the child is stopped and the
  request not sent again (at idle priority a busy machine can starve the child that long, so the next request gets a
  new child).
- `carry_translate()` translates a stored input again under the current identity and stores the files as
  `translate_one()` does, leaving the memory cache alone. The job's files have names of their own (sha256("TKSC",
  key)), so an on-demand translation of the same key at the same time never writes the same files; each renames its own
  onto the key's, with the same content. 1 stored, 0 the translator refused the program, gave no answer or wrote no
  usable translation (`why` says which), -1 no translator or a file error. As in `translate_one()`, a refused or
  unanswered job and its code stay, for a translator fix to be tested on. With no answer this input fails and the pass
  goes on with the next on a new child. When the rename fails, a reader has the key's files open: an on-demand
  translation stored them meanwhile. `use_store()` is called at normal priority: it takes the cache lock the render
  thread reads under.
- `Drops`: `v` the packed files to leave out of the pruned pack; `deleted` the loose files deleted; `left` the files
  still there (a delete failed, or packed).
- `drop_file()`: one file of a previous translator: the loose file is deleted, a packed one is listed for the pruned
  pack. A loose file that is open in a reader stays, and the next pass deletes it.
- `carry_prune_one()`: previous identity `j`'s files of the inputs the pass covered: their `.spv` and `.res.json`, and
  the `.use` of each program none of whose other inputs still needs it. The inputs it did not cover (refused, skipped,
  not reached) keep theirs, so `carry_find()` still serves them. It returns how many of `j`'s entries remain (packed
  ones until the pruned pack is in place); 0 drops `j` from `identities.txt`.
- `carry_repack()` writes `shader-cache.pack.pruned`: the open pack without the dropped files, with a trailer naming
  the pack it was copied from (`pack_swap_pruned()` puts it in place at the next start; the open pack cannot be
  replaced while this process reads it). Written under another name, flushed, then renamed, so a copy that exists is
  whole. 1 when it was written. When a copy of this pack already waits for the next start (its swap failed, or this
  pass ran before), the whole pack is not copied a second time; the pass after the swap prunes what it left in. The
  data starts at 16 + the index's bytes; the buffer is written out when it is full, and between writes on-demand
  translation goes first.
- `carry_main()` first deletes a copy an earlier process did not finish. It lists the stored inputs a previous
  translator has an entry of. `req_init()` and `in_load()` run at normal priority: `req_init()` takes the cache lock
  the render thread reads under. An input the current translator has already is covered. Then come the entries the pass
  no longer needs: in this process each input it covered now has a current key on the disk whose use mask is known, and
  lookups try that before a previous translator's disk entry; the shaders already served stay in memory as they are.
  The cache then holds the current translator's translations of what the pass covered: the current identity stays
  listed, first, even when this process stored or read nothing else under it.

### `gfx_shader_ram()`

The memory report's figure for the shader caches in process memory (`system_log()` of `runtime/vk/vk_memory.c`), at
report time: the memory cache's translated shaders (entries with their SPIR-V, reflection text and resource lists, kept
for the life of the process), the use masks (those read at the start, `use_preload()`, and those found since), the
cache pack's index, and the records of programs and answers met in play (their sets and what waits to be appended).
The walk holds `cache_lock` shared, as `gfx_shader_sizes()` does: a translator's insert waits for it. The SPIR-V is
counted as it is held (a plain copy, a compressed frame or both), read without `spv_lock`: a store the SPIR-V thread
changes meanwhile is counted as it was or as it is.

### `gfx_shader_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`perf.c`): the shader memory cache's entries with their SPIR-V as held (plain and
compressed, `gfx_shader_ram()`) and reflection bytes (kept for the life of the process), the remembered failures, the
translation and load queues, and the translator processes. `cache_lock` is held shared: lookups go on, a translator's
insert waits for the walk.

## `runtime/gfx/gfx_present.c`

The Vulkan surface and swapchain, the present blit, the 60 Hz vsync thread and the focus: the half of the window that
knows no window system (the other half is `gfx_window_win.c` with Win32 or `gfx_window_sdl.c` with SDL, through
`runtime/gfx/gfx_wsys.h`). The file also holds the queue lock, the steps of an orderly end, the movie overlay and the
frame generation presenter. The licence notice in its third and fourth lines stays in the code.

Threads: the window thread (the back end's) owns the window and never runs guest code. The vsync thread signals the
bound system events. Presents come from any host thread (the back end's present worker) and take `g_pl`; every queue
submission takes the recursive queue lock (`vk_queue_lock()`). The swapchain is rebuilt lazily: a resize only marks it
dirty (`gfx_window_mark_dirty()`), the next present (or `gfx_window_resize()`) rebuilds it.

The in-game menu (`runtime/ui/menu.h`) draws over the game image in the present's command buffer, after the blit. The
window's size and fullscreen state are the settings `NX2SARIA_WINDOW_SIZE` and `NX2SARIA_FULLSCREEN`, which the back
end applies at start and whenever a `cfg_set` changes them (`window_settings_live()`, then
`gfx_window_apply_settings`). The window keys: [controls.md](../controls.md#window-keys).

`NG_MAX` (5) is the most generated frames per rendered one (DLSS Frame Generation through NGX, 6x).

### `vk_queue_lock()` `vk_queue_lock_raw()` `vk_queue_trylock_raw()` `vk_queue_unlock()`

Taking the queue lock hands the replay's held submissions to the queue first (`runtime/vk/vk_sync.c`, submit
batching), so whatever the holder submits or waits for comes after them in queue order.

`NX2SARIA_LOCK_TIME` also names who holds it: a hold of `QL_HOLD_NS` (5 ms) or more goes into the frame-time summary
under the site that took the lock (`perf_qlock_hold` in `perf.c`), beside the waits for it. A present had waited 48 ms
for the lock with no holder named anywhere. The taking functions are not inlined, so that their return address is the
caller's site, which is also the site the waits are reported under (`plat_rmutex_lock_at`). `ql_site` and `ql_t0` are
written by the holder only, at the outermost take and release.

### Window state shared with the back end

`g_dirty`: the window was resized and the swapchain is stale. `g_restart`: `gfx_window_request_restart()`, closing the
window starts the game again. `g_start_lock` guards `gfx_window_start()` and the vsync start.

### `gfx_window_focus_changed()`

The window gained or lost the focus: the game hears it as `nn::oe`'s focus state and a notification message
(`oe_set_focus` is pinned to 1 by `NX2SARIA_ASSUME_FOCUS` inside). The line carries the local wall time with its date,
the first in the log: when a GPU hang takes the whole system down, the log ends without an error, and this time set
against the driver's entry in the Windows event log (nvlddmkm 153, Display 4101) tells whether an alt-tab or an overlay
came before the hang or followed it. The lines that mark a stall carry the time of day alone (`log_stamp` in
`log_stall.c`); the date is this line's.

On a loss of focus the session's reports and the log are written now (`perf_focus_lost` in `perf.c`,
`rt_log_focus_lost` in `main.c`), should the process be ended from outside next.

### `rt_exit_begin()` `rt_exit_started()`

The orderly end: the window's close (`gfx_window_closed()`), the guest's ExitProcess (`runtime/hle/svc.c`) and the end
of a `--seconds` run (`rt_frame_presented` in `runtime/boot.c`). Once per process, whichever comes first. The window's
close and the console's (`console_closed` in `main.c`) can come together, or the guest's ExitProcess beside either:
each ran its own sequence, so two pipeline saves and two frame-time summaries were written while both raced.
`rt_exit_begin()` returns 1 for the first caller; a caller on another thread waits there for good while the first ends
the process; 0 for the first caller's own thread coming back, which goes on with the sequence it is in. The window's
close runs on a thread of its own (`exit_start` in `runtime/gfx/gfx_window_win.c`); the window thread runs the sequence
itself, and may handle a message inside it, only when that thread could not be made.

`exit_tid` is the thread that runs the orderly end, 0 before one started. The first caller calls `rt_log_ending`: the
end's lines reach the log file past its cap (`log_sink` in `main.c`).

### `rt_exit_steps()`

What every orderly end saves and puts back, in this order. What a kill of the process must not lose and what is quick
comes first: Windows ends a closed console about 5 s after the close, a session's end and an "End task" sooner, and the
pipeline save alone can take that long (`vk_pipeline_save_exit` waits up to 5 s for the builds that run). So a Commit
in flight is finished, RivaTuner Statistics Server's profile and the controller's rumble are put back, the settings,
the boot prebuild's marker, the frame-time summary and the memory ranking are written, and the log reaches the disk
before the pipeline save starts. The final save flush comes last: the game's threads run until `plat_exit`, and a
Commit the game starts during the pipeline save is still taken and waited for. `RT_EXIT_SETTINGS`: the settings file
gets the menu's changes (not in a scripted `--seconds` run, whose end is no player's). The file is `nx2saria.ini`.

The steps:

1. `fs_save_wait`: a Commit already under way is finished, nothing refused.
2. `rtss_limit_restore`: the game's frame limit out of the profile; `rtss_limit.c` keeps it out while frames go on
   during the pipeline save.
3. `input_rumble_hd_stop`: a controller on HD rumble gets the neutral value; nothing renews or stops it after the
   exit.
4. `cfg_save`, with `RT_EXIT_SETTINGS`: a menu change saves 400 ms later on the render thread (`option_rows.cpp`).
5. `vk_pipeline_prewarm_marker_clear`: the game ended while the boot still built its pipelines, which is not the
   driver crash the marker is there to catch. Left in place, the next start took it for one and every later start
   skipped the prebuild (`pm_skipped` in `runtime/vk/vk_pipeline.c`).
6. `prof_write`: "frametime: exit" and the stutter map; `plat_exit` runs no atexit handlers.
7. `session_summary_exit`: `logs/session-summary.json`, from the session's frame times `prof_write` closed.
8. `vk_mem_exit_log`: the memory report's ranking of the session.
9. `heap_retry_exit_log`: the model heaps' retries of the session.
10. `changebank_hold_exit_log`: the neighbouring map preloads held.
11. `camera_clearance_exit_log`: what the camera's clearance loaded.
12. `cheats_exit_log`.
13. `vk_exec_mfb_batch_exit_log`: the mesh fallback batch's totals (`runtime/vk/vk_exec_draw.c`), with it on.
14. `logq_flush`: the lines so far on the disk before the slow part; the queue stays on.
15. `vk_pipeline_save_exit`.
16. `fs_save_flush`, last: a Commit started since `fs_save_wait` finishes, and one after this is refused.

### `gfx_window_closed()`

The window was closed: the process ends with it, once a save Commit still running in the background is done. After
`gfx_window_request_restart()` the executable then starts again with the same command line and environment
(`plat_restart`), and the new process waits until this one is gone (`main.c`) before it moves the log and opens the
RomFS and the saves. `NX2SARIA_REEXEC` is cleared for it: the new process counts its own address-space retries
(`runtime/memory.c`).

### `gfx_cfg_fullscreen()` `gfx_cfg_window_size()`

`NX2SARIA_FULLSCREEN=1`: a borderless window over the monitor. `NX2SARIA_WINDOW_SIZE=<w>x<h>`: the client area of the
window (the swapchain follows the window; the game image is scaled to it). A value that cannot be parsed or is out of
range gives the setting's default (1920x1080 when that cannot be read either).

### `gfx_window_set_refresh()` `gfx_window_refresh_hz()`

The refresh rate of the window's monitor (`NX2SARIA_FPS_LIMIT=display`), measured by the back end's window thread when
the window opens, lands on another monitor or the display mode changes; readers take the stored value. `g_rate_mhz`
holds it in millihertz, 0 unknown.

### Swapchain and present state

All under `g_pl`.

- `g_sc_hdr`: HDR10 (`NX2SARIA_HDR`, `runtime/vk/vk_hdr.c`): PQ code values in Rec. 2020.
- `g_scaler`, the last present: 0 the game image 1:1, 1 scaled by the blit, 2 by FSR, 3 shrunk by the blit and
  sharpened by RCAS.
- `g_sc_view`: the colour attachment views for the menu overlay.
- `g_flip_on`: `vk_present_blit()` submitted the blit of swapchain image `g_flip_idx`, and `vk_present_flip()`
  presents it; `g_flip_frame` is that blit's game frame, when a driver's low latency follows it (`vk_lowlat.c`), else
  0.
- `g_pic` is what `gfx_picture_rect()` returns.

### `pts_begin()` `pts_end()` `pts_read()`

The GPU timeline (`NX2SARIA_TRACE=perf`, `vk_gtl_note` in `runtime/vk/vk.h`): a timestamp pair per present slot around
the present's command buffer, read once the slot's fence is waited for again (`pts_read()`: then its last present is
done). The gap before it shows a wait for the acquired image (the submission waits on the acquire semaphore) as idle
time with work queued before presents. `pts_state` is 0 not set up, 1 on, -1 off; `pts_on` says the slot's pair was
written by its last present; `pts_sub` when that present went to the queue.

### The flight recorder's thumbnails

`NX2SARIA_FLIGHT` (`flight.c`; under `g_pl`): each present also blits the swapchain image down into slot
`frame % g_th_n` of a host-visible ring. A slot's tag names the frame it holds complete; recording a new frame clears
the tag first, and the tag is set once the fence of that present has signalled. `g_th_frame` is the frame of the next
`vk_present_image()` call (`vk_thumbs_frame()`); `g_th_tag` is per ring slot; `g_th_pending`, per fence slot, the
frame whose thumbnail its submission writes.

- `ensure_thumbs()`: the ring and the blit target in the swapchain's format; allocated on the first present, the image
  again when the swapchain format changes. A failure turns the thumbnails off for the session. An HDR10 swapchain blits
  into RGBA8: its PQ codes, flat-looking but readable as bytes. The old image can be destroyed at once: the previous
  present's fence was waited for, so it is idle.
- `record_thumb()`: `dst` (TRANSFER_SRC, the swapchain image just written) to the thumbnail image to the ring slot of
  `g_th_frame`. The slot's tag is cleared under `g_pl`, so `vk_thumbs_read()` no longer hands the slot out.
- `thumb_cells()`: the flight recorder's flash trigger (`flight_cells` in `flight.c`) gets the cell means of the
  frame's thumbnail, which has just completed in its ring slot (`g_pl` held). `flash_cells` reads a quarter of the
  slot's pixels, about 15 us measured on ordinary memory (`tests/flight_flash_check.py time`); the ring is mapped host
  memory allocated for random access (`ensure_thumbs()`), which is cached memory where the device has it, as
  `vk_thumbs_read()`'s copy of a whole slot relies on.

### `vsync_main()` `gfx_vsync_bind()`

In `vsync_main()`, when the process stalled (more than 100 ms past a tick's due time) the schedule restarts, with no
catch-up burst.

`gfx_vsync_bind()` lists a vsync event the vsync thread signals. With the list full, an event the game destroyed since
(`kern_system_event_live` 0: its state byte is 0 or its memory is gone) gives its slot, which also stops the vsync
thread signalling it; the list ends the process only when all 16 are live. Each new listing is logged, so a long
session's log shows how often the game asks for a vsync event at a new address.

### `window_settings_live()`

On the present path (`g_pl` held): a settings change (`cfg_version`) reaches the window thread, which applies a changed
size or fullscreen; a changed `NX2SARIA_VK_PRESENT` or `NX2SARIA_HDR` rebuilds the swapchain at this present.
`g_sc_hdr_wanted` is `vk_hdr_wanted` when the swapchain was made (`create_swapchain()`), `g_sc_ll_wanted` is
`vk_lowlat_swapchain_wanted` then, and `present` the `NX2SARIA_VK_PRESENT` the swapchain was made with.

- Frame generation that failed (`fail` in `runtime/vk/vk_fg.cpp` makes `vk_fg_wanted` 0 with no setting changed): its
  swapchain and both contexts go at the next present, not when some other setting moves. Only this way round at every
  present: a frame generation swapchain wanted and not made is tried again at a settings change, not at every frame.
- Frame generation switched: its own swapchain.
- HDR switched in the menu: a swapchain of the other format. Compared with what the swapchain was made for, not with
  what it got: on a display without HDR10 the setting stays on and the swapchain SDR, and that is settled.
- NVIDIA's low latency switched in the menu (`vk_lowlat.c`): its mode is asked for when the swapchain is made.

### `pick_present_mode()` `gfx_window_present_mode()`

`g_present_got` is the `NX2SARIA_VK_PRESENT` value of the mode the swapchain got; "" before one.

Mailbox missing (several AMD drivers): plain FIFO holds a frame that misses a refresh to the next one, and the game,
paced at 60 on a 60 Hz display, drops to 30 at every slow frame. Relaxed FIFO shows a late frame at once (a tear that
once) and waits for the refresh otherwise; immediate where that is missing too. The mode last logged is kept, so a
change from the menu shows in the log.

### `gfx_window_hosted_ns()`

`kern_now_ns` when the newest host present returned with its picture taken (`flip_locked()`); 0 before the first. A
minimised window and a frame hold present nothing, so it ages: RTSS, which holds the frames inside the host present,
then holds none, and the game's pacer takes the limit back (`rtss_pace_period` in `rtss_decide.h`).

### `flip_locked()`

Queues the present of a blit `vk_present_blit()` left waiting (`g_pl` held). A swapchain rebuild and the next blit
flush it first; the device release drops it.

A driver's low latency (`vk_lowlat.c`): its present stage or marker right before the present of a game frame it
follows, the frame's number on the present where the plain swapchain takes one, and the end marker after it. Nothing
for any other present (the boot screen, a frame the mode did not start, the mode off).

### `drop_swapchain_locked()`

Destroys `g_sc` (when there is one: the frame generation paths destroy theirs themselves) with its image views and its
images' render-done semaphores; the caller holds the queue lock and the device is idle. The swapchain goes before the
semaphores: a present it still holds may wait on one of them.

### `swapchain_unusable()`

The swapchain `create_swapchain()` just made cannot be used: its image list (`runtime/gfx/gfx_guard.h`
`gfx_sc_images_ok()`) or a semaphore for one of its images failed. A present with it would name images or semaphores
that are not there, so it goes with what was made for it, and the next present makes one again (`g_dirty`). `call`
names the step that failed, `r` its result, `detail` the counts; `*count` is that step's failures, logged as
`gfx_fail_log_due()` allows: "gfx: <call> failed (<r>), <n> times: <detail>". The caller holds the queue lock, which
this releases. It returns 0, `create_swapchain()`'s failure. AMD's swapchain has its own destroy.

### `surface_formats()`

Surface-format lists can exceed 64 entries when a compositor advertises several colour spaces. The query is made again
on VK_INCOMPLETE: the available list may grow between the count and data calls.

### `create_swapchain()`

Returns 1 with a usable swapchain, 0 when the window has no area (minimised) or the create failed, which leaves
`g_dirty` set so the next present tries again. Called with `g_pl` held. `g_dirty` is cleared before the window's size
is read: a resize that arrives while the swapchain is being made (AMD's frame generation swapchain takes tens of ms)
sets it again, where clearing it at the end lost that resize and kept a swapchain of the old size, whose acquires then
timed out frame after frame behind AMD's presenter (the picture stopped).

- First `flip_locked()`, so the acquired image goes out before its swapchain is replaced, and `ng_drain()`, for what
  the NGX frame generation presenter still holds (nothing unless it runs).
- The surface format list's size is logged once per change of it, not at each rebuild (a resize, a mode change).
- The format is UNORM (B8G8R8A8 with the sRGB non-linear colour space): the blit writes the bytes the game wrote (no
  sRGB encode). `NX2SARIA_HDR`: HDR10 when the surface offers it. With HDR off, or not on this display, what its encode
  made goes (`vk_hdr_images_release`).
- NGX frame generation's presenter holds the generated frames' images too, and the rendered frame waits for it: room
  for both (`minImageCount + 1`, plus 2 + `NG_MAX` with NGX; at most 12 with it and 8 without).
- A rebuild holds the queue lock across the device's idle wait and the create, and every submission waits behind it:
  the made swapchain's line says how long (the first 16 and each doubling of the count).
- NVIDIA's low-latency calls name the swapchain: none while it is replaced (`vk_lowlat_swapchain`).
- The image usage includes COLOR_ATTACHMENT: every surface supports it (the menu overlay draws into it).
- Frame generation (`runtime/vk/vk_fg.cpp`): AMD's swapchain in this one's place. Its swapchain is replaced only after
  the old one is gone; a first creation that fails leaves the plain swapchain. Switched off in the menu, AMD's
  swapchain and contexts go and the plain one follows. Once AMD's swapchain context exists every swapchain call goes
  through it: a plain swapchain in its place would get AMD's acquire and present, so a failed create is tried again at
  the next present instead (after the old swapchain's views and semaphores are dropped).
- NVIDIA's low latency (`vk_lowlat.c`): the plain swapchain is made with the latency mode while the setting is on, the
  only structure in its chain. A frame generation's swapchain is not: the calls cannot name it. With DLSS Frame
  Generation active the create goes through Streamline's hook (`vk_dlssg_create_swapchain`). Three creates in a row
  that fail with the latency mode asked for: the next is made without it.
- A create with `oldSwapchain` retires the old swapchain even when it fails: it can no longer acquire or be an
  `oldSwapchain` again. It goes now (the device is idle), and the next present creates one from nothing. `g_sc` is set
  to the old one for the drop, NULL when a frame generation path destroyed it already: its views and semaphores still
  go.
- The log says when a later swapchain is of the other kind, HDR10 or SDR (the menu's HDR row).
- `vk_lowlat_swapchain(..., 1)`: the sleep mode is set on the swapchain; none when it is not one for that path.
- The image list, the count first and then that many images, on every path (AMD's swapchain does not cut a larger
  capacity down to its count). An acquire returns an index below the count, and every array here has `MAX_IMAGES` (16)
  slots: a list that did not come whole (an error, VK_INCOMPLETE, fewer images than counted, more than the arrays hold)
  leaves the swapchain unused rather than presenting with images that are not there. A null image in a list that came
  whole is a broken list: an acquire may name it, or one past it.
- Each image's render-done semaphore: the present of that image waits on it, so an image without one cannot be shown at
  all (the handles not made are null, which `drop_swapchain_locked()` skips).

### `vk_present_swapchain_mem()`

The memory report's swapchain (`outside_log` in `runtime/vk/vk_memory.c`): its images and their size; the bytes are the
pixels alone, 8 a pixel for a half-float format and 4 for the others (the driver's padding and compression data are not
known). Read without `g_pl`: the figures of a swapchain made again meanwhile are a report late. The images of a frame
generation's swapchain are the ones counted (`create_swapchain()` takes them from it).

### `vk_present_grab()` `grab_deliver()`

`vk_present_grab()` (`runtime/vk/vk.h`): one presented frame's pixels for a bug capture. The request is taken by the
next present (`g_pl` held), which copies the read-back buffer and calls back; the PNG is made by the caller's own
thread. `grab_deliver()` runs in the present that took the request: the read-back buffer holds this frame (`g_cap_ptr`,
the fence waited for). The callback gets NULL when there is no memory for the copy.

### `save_capture()`

The captured swapchain image as a PNG (the menu's test hook), RGBA with opaque alpha. From an HDR10 swapchain the log
also gets the luminance in nits of the picture's rectangle (`vk_hdr_luminance_stats`: the black floor of a dark scene,
for one), and `<path>.pq10` keeps the raw pixels: "PQ10" (0x30315150), width, height, 1 when A2R10G10B10 (u32 each),
then the pixels as the swapchain holds them.

### `gfx_fit_rect()` `gfx_picture_rect()`

`gfx_fit_rect()`: the rectangle {x, y, w, h} of a `dw` x `dh` swapchain image the `cw` x `ch` picture goes into: the
whole image with `NX2SARIA_ASPECT=stretch`, else (fit, the default) the largest one of the picture's own aspect ratio,
centred (a 16:9 picture on the Steam Deck's 1280x800 screen gets bars of 40 rows above and below; a 16:9 picture in a
21:9 window, bars at the sides). A window wider than the picture gets a pillarbox, a taller one a letterbox. A side
takes the window's parity, which keeps the two bars equal; a picture one pixel short of the window fills it.

`gfx_picture_rect()`: where the last present put the picture, as fractions of the swapchain (x, y, w, h; `g_pic`), for
the pointer of `menu_cursor.c`.

### `gfx_movie_frame()` `movie_overlay()`

The movie overlay (`runtime/hle/ov_movie2.c`): the latest decoded movie frame goes over the game image, fit to the
window with black bars, while frames keep coming (`GFX_MOVIE_HOLD_NS`, 400 ms, after the last one it disappears). It
made movies visible before the game's own shaders translated; now the game draws them itself (its YUV compute pass,
then the upscaler fallback of `runtime/vk/vk_exec_draw.c`), under its menus, so the overlay is off unless
`NX2SARIA_MOVIE_OVERLAY=1`. One staging buffer per frame in flight: the CPU writes slot i only after that slot's fence
(the present waits it before recording), the GPU copies it into one RGBA8 image.

`movie_overlay()` records the overlay into `cb` for swapchain image `dst` (in TRANSFER_DST_OPTIMAL, left so); 1 when
drawn. The game image's blit wrote `dst`: black bars first, then the frame.

### `put_picture()`

The game image's crop into `dst` at `to`: colour adjustment (or the HDR10 encode), then FSR or the bilinear blit; `dst`
in TRANSFER_DST_OPTIMAL. It returns `vk_fsr_record`'s result (0: the blit scaled it).

- Colour adjustment (`NX2SARIA_COLOR_*`, `runtime/vk/vk_color.c`), or on an HDR10 swapchain the PQ encode
  (`runtime/vk/vk_hdr.c`; the adjustment is for SDR): a copy of the crop takes the game image's place.
- FSR (`NX2SARIA_UPSCALER`): EASU when the picture's rectangle is larger than the crop, the shrink and RCAS when it is
  smaller; else the bilinear blit.
- For a GENERAL source (the images of `runtime/vk/vk_texture.c`) a barrier makes earlier submissions' writes visible to
  the blit, and one after it makes later submissions that write the source wait for the blit's read.

### `output_rate()` `gfx_output_fps()`

The display's frame rate with frame generation: the frames the display got (generated ones included: `vk_fg_presents`
of `runtime/vk/vk_fg.cpp`, `vk_dlssg_shown` of `runtime/vk/vk_dlssg.c`) over half a second, sampled at each of the
game's presents. `ng_presented()` gives the frames the NGX frame generation presenter showed (0: it does not run) and
`ng_running()` says that presenter is in use.

### `hudless_record()`

`g_hl` is frame generation's HUD-less picture at the swapchain's size.

The upscaler's output can be smaller than the presented picture: the scene is 75% of it in handheld mode
(`widescreen.c`, 1440x810 in a 1920x1080 picture) and the game's last passes scale it up with the HUD. Taken only at
the same size, as before, that frame had no HUD-less picture and was not generated: 5 of 3600 frames generated in
handheld mode on an RX 580 (2026-11-11). The crop is taken at the same place of the smaller picture.

The steps' work images were just read for the presented picture: they are written again only after that. The steps'
per-slot views of their source get a slot of their own (`slot + FRAMES`; the presented picture's views stay valid for
this command buffer); with HDR the PQ encode runs as for the presented float picture.

### The frame generation presenter

How many frames get a generated one, and what was wrong before: [frame-generation.md](../frame-generation.md).

Two generators run through it, in every build but the Windows one (none of this is reached there): DLSS Frame
Generation through NGX (`vk_ngxfg_*`, `NX2SARIA_FRAMEGEN = dlss` where Streamline does not run) and AMD FSR 3.1's frame
generation through the runtime's own passes (`runtime/vk/vk_fsr3fg.c`, `NX2SARIA_FRAMEGEN = fsr3`; one generated frame
per rendered frame, at the middle). `ng.src` names the one in use; the log lines carry its name (ngxfg, fsr3fg). Each
frame is composed into an image of its own (`ng.bb`) instead of a swapchain image. At the end of the frame's command
buffer DLSS Frame Generation makes the generated frames from it (`vk_ngxfg_evaluate`, the multiplier less one) and the
picture is copied aside. The presenter thread then shows them in order, each copied into a swapchain image it acquires
itself, and the rendered frame last, spaced evenly over the time between two rendered frames, as NVIDIA's frame
generation guide paces them. Two sets of images take turns: a set is written again two frames later, after the
presenter's copies from it are done (the timeline `ng.copied`). Every frame goes through the presenter while it runs,
so the timelines advance in order.

Three things froze the picture while this was written, and the code keeps away from each:

- a frame's own command buffer never waits for a swapchain acquire: on Wayland an acquire's semaphore can signal only
  after a later present, and every later present would come from that command buffer;
- a timeline wait is never submitted ahead of its signal on the same queue (`ng_wait_signaled()`);
- an acquire never blocks while it holds the queue lock the presenter presents with (`ng_acquire()`).

The fields of `ng`: `tried` 0 nothing made, 1 the objects exist, 2 it cannot run here; `thread_up`, the presenter
thread runs (started once, kept over a release); `rendered` and `copied`, the timelines (frame n's command buffer
submitted or done; the presenter done with frame n); `n`, the frames handed to the presenter; `cb[2][NG_MAX + 1]`, per
shown frame, the generated ones and then the rendered one; `signaled`, the last value of `ng.copied` the presenter
submitted a signal for (under `ng.m`); `last_t` and `interval`, the average time between rendered frames (ns);
`presents`, the frames presented while it runs, generated ones included; `src`, the generator in use (`NG_DLSS`,
`NG_FSR3`), 0 none; and the pacing, on the presenter thread: the time of the last present, and the gaps between
presents against the even spacing the presenter aims at (the time between rendered frames over the frames shown for
each).

- `ng_acquire()`: an acquire that never blocks while it holds the queue lock: the swapchain is shared by the game
  thread and the presenter (both acquire, the presenter presents), and an image is freed only by a present, which needs
  that lock. A blocking acquire under the lock waited for the present it held off: the picture froze (1 s acquire
  timeouts).
- `ng_drain()` waits until the presenter has shown everything handed to it.
- `ng_pace()`: one present's gap from the one before against the even spacing (`ideal_ns`); a gap over 100 ms (a pause,
  a load) is not counted. A gap longer than 1.5 times the spacing is a pacing drop: the eye sees that frame held.
- `ng_signal_copied()` signals after everything submitted before it (frame v's copies included).
- `ng_wait_signaled()`: a frame's submission waits for `ng.copied >= n - 2` on the GPU. That signal must be submitted
  first: one queue runs its submissions in order, and a wait queued ahead of its signal stopped the queue for good (the
  picture froze).
- `ng_show()` acquires a swapchain image, copies `img` (GENERAL, the swapchain's size and format) into it once frame
  n's command buffer is done (`ng.rendered`), and presents it. Only the presenter acquires while it runs: the frame's
  own command buffer never waits for an acquire, whose image the compositor may give back only after a later present.
  A failed acquire has a line with the time the first few times; the frame-time summary counts them all
  (`log_stall.c`).
- `ng_run()`: with n > 2 it first waits for the last use of this set's command buffers and acquire semaphores (frame
  n - 2) to be over. The first generated frame is shown at once (the least latency), the others at even steps, and the
  rendered frame last; then `ng_signal_copied(n)`, after the copies out of this set. The line it logs (at 60 jobs and
  every 1200) carries the most of each period for the largest gap, and for FSR 3.1 the generator's own counts, read
  there on the presenter thread: a few frames old.
- `ng_thread()`: with nothing for 1.5 s it logs where the GPU stands (the two timelines' values). The first few have a
  line; the frame-time summary counts them all.
- `fsr3fg_state_line()`: one line at the first present and at each change: whether AMD FSR 3.1's frame generation
  through the runtime's passes runs, at what multiplier, or why not (the setting, an HDR swapchain, a device that
  cannot run the passes, a failure).
- `ng_ready()`: the presenter and the images for the current swapchain (`g_pl` held). 0: frames go out the plain way.
  The Windows build has neither generator here (AMD's library and Streamline make their own swapchains there); NGX's
  needs a run that started with DLSS upscaling (`vk_ngxfg_wanted`), FSR 3.1's the device's passes (`vk_fsr3fg_init`).
  `vk_dlaa_hudless_wanted` is set for the HUD-less picture both tell the HUD apart by. Switched off live in the menu,
  what the presenter holds goes out first. When the menu switched from one generator to the other, each starts anew,
  and the images are made again for the new generator's count of generated frames. The presenter's semaphores, command
  pool and command buffers: one that is not made turns frame generation off for the run, as the checks before it do,
  with what was made released: the presenter would otherwise wait on or submit null handles. That is logged once a run
  (`ng.tried = 2`), counted all the same. The command pool takes its command buffers with it. FSR 3.1 generates one
  frame per rendered frame: one image a set, where DLSS Frame Generation may need `NG_MAX`.
- `ng_multiplier()`: `NX2SARIA_FG_MULT`: 2 to 6, or dynamic: only the frames needed to reach the display's refresh rate
  at the rendered frame rate (240 Hz at 60 fps: 4x), between 2x and what the GPU allows, as NVIDIA describes its
  dynamic multi frame generation; a new multiplier must hold for 30 frames before it is taken, so it does not flicker.
  `fit` is the shown frames per rendered one that fit.
- `ng_record()` records the copy of the frame's picture and the generator's frames (the end of `present_image()`'s
  command buffer, `dst` in layout `*last`; `pic`: the picture's rectangle in it). It returns the number of generated
  frames, -1 when the frame goes out the plain way. With `ng.fresh`, every image goes to GENERAL, once (FSR 3.1's sets
  have one output image). The frame's picture is in `dst` (`ng.bb`): to GENERAL for DLSS-G and the copy into this set's
  rendered-frame image. Generation pauses while the menu is open. The rendered frame is copied for the presenter
  (`ng.bb` is drawn again by the next frame).
- `ng_release()` (`gfx_window_release_vk()`, the device idle): the presenter shows what it holds, then its images,
  semaphores and command buffers go with the device. The thread stays and waits; a later device makes the objects again
  (`ng_ready()`). The device is waited idle for what the presenter submitted while it drained; new timelines start at
  0.
- `ng_hand()` hands the submitted frame to the presenter (it shows `count` generated frames, then swapchain image
  `idx`).

### `present_image()`

- The presented image's format is logged when it changes (an HDR picture comes in another one).
- With the device lost the run is ending (`vk_device_lost_check` in `runtime/vk/vk_device.c`): nothing touches the
  device.
- The present slot's fence wait: a lost device ends the run here as anywhere else; a hung GPU times out at every
  present, so the line is kept to the first 8 and each doubling.
- `pts_read()`: the GPU timeline, this slot's last present is done.
- NGX frame generation: the frame is composed into `ng.bb` and the presenter shows it (`ng_show()`); no acquire here.
  The presenter presents several times a frame, so a driver's low latency steps aside (`vk_lowlat_fg_presents`).
- Not ready: the frame is skipped. Acquires that keep timing out (1 s each) mean a swapchain nothing will free an image
  of again: three in a row rebuild it, where the picture used to stop for good. An error (the surface lost, say) is
  logged once per code.
- With HDR, a changed peak or white goes to the display (`vk_hdr_metadata`).
- A pending thumbnail whose fence has signalled is complete, and its tag is set.
- The game's crop (nvnWindowSetCrop, guest values) is cut to the image without overflow (`gfx_crop_clip()` of
  `gfx_guard.h`); a crop outside the image is the whole image, not an inverted blit rectangle. A crop that had to be
  cut or replaced is logged with its values, as `gfx_fail_log_due()` allows.
- The menu overlay is laid out before recording (its first frame starts the ImGui back end and uploads the font).
  Without a source image (`gfx_present_boot()`): a black frame with the boot screen.
- `NX2SARIA_SHOT_EVERY=<s>` with `NX2SARIA_SHOT_DIR=<dir>`: the presented frame as `shot_<n>.png` every s seconds (a
  check of what the window shows, for runs nobody watches). `NX2SARIA_SHOT_AFTER`: seconds after the start before the
  first shot.
- `grab`: a bug capture asked for this frame (`vk_present_grab()`).
- `to` is where the picture goes in the swapchain image, {x, y, w, h}; black bars beside or above and below it are
  cleared when it does not fill the image.
- Frame generation without the HUD in its generated frames: the picture as it would be without the HUD
  (`runtime/vk/vk_dlaa.c`, DLSS's or FSR 3.1's output before the HUD was drawn over it) goes through the same steps
  into an image of the swapchain's size, which frame generation compares with the presented one to find the HUD and
  leave it unwarped. `NX2SARIA_FG_HUDLESS_SHOW=1` (a test) puts the HUD-less picture on screen instead.
- A movie frame goes over the game image on SDR swapchains.
- `last`, `last_stage` and `last_write`: the image's layout, and the stage and access of its last write, as each step
  leaves it.
- Centered UI and wide movies and fades use the middle 16:9 ([wide-hud.md](../wide-hud.md)). The outside margins are
  cleared after composition, leaving the HUD-less frame-generation source and the menu pointer (`g_pic`) at the whole
  picture's coordinates. Tall screens fill the height.
- HDR10: the menu goes into `vk_hdr.c`'s RGBA8 image, which is then blended onto the swapchain image at the HUD's
  white.
- `vk_fg_frame`: frame generation, this frame's preparation. `vk_dlssg_frame`: DLSS Frame Generation's constants and
  tags (`runtime/vk/vk_dlssg.c`). Through the presenter the picture stays in `ng.bb`, and DLSS Frame Generation's
  frames and the copy for the presenter are recorded; otherwise the swapchain image is ready to present.
- A driver's low latency (`vk_lowlat.c`): the game frame this blit shows, and NVIDIA's tag with its number.
- Through the presenter (`ng_n` is this frame's number on its timelines) there is no acquire to wait for. The frame
  writes one of the presenter's two image sets: it waits on the GPU until the presenter is done with the frame before
  last, which used that set, and that signal is submitted first (`ng_wait_signaled()`). It signals the timeline the
  presenter's copies wait for instead of a render-done semaphore. The presenter then shows the generated frames, then
  this one.
- When the submit failed: the fence was reset after the acquire and the acquire's semaphore is signalled: left so,
  this slot's fence wait would time out (5 s) at every later present and the picture stop for good. An empty batch
  takes the semaphore and signals the fence; the acquired image goes back with the swapchain, made again next frame.
  (Through the presenter nothing was acquired: the batch only signals the fence.)
- When the wait for a captured frame failed, the read-back buffer may not hold this frame yet: nothing reads it. With
  no read-back buffer a grab request ends without pixels.

### `gfx_present_boot()`

The boot screen on a black frame (`menu_boot_frame`), without a game image: `present_image()`'s paths that read a
source are skipped. The size (1920x1080) only places the picture rectangle, which a frame without a picture leaves as
it was.

### `gfx_window_release_vk()` `gfx_window_start()`

`gfx_window_release_vk()` is called by `vk_shutdown` before the device goes away; the window and its thread stay. A
waiting flip is dropped: its blit is done (the device is idle) and the swapchain goes away unpresented. NGX frame
generation's presenter shows nothing more, and its images go before the device; FSR 3.1 frame generation's passes and
images go too, made again for a later device. The next `gfx_window_start()` makes a new surface.

`gfx_window_start()` makes the frame slots' command pool, command buffers, fences and acquire semaphores: every present
uses them, so one that is not made ends the start with its call and result, where a null handle used to fail later
inside the driver.

### `gfx_lock_holders()`

For the hang watch's report (`hang_watch.c`; [performance.md](../performance.md#the-hang-watch)): who holds the
present, queue and window-start locks now, read without taking them (0: free). The fields may be caught mid-change; the
report only names them.

## `runtime/gfx/gfx_window_sdl.c`

The SDL3 back end of the window (`runtime/gfx/gfx_wsys.h`), for hosts without Win32: the window on a thread of its own
that also runs SDL's event loop, F11 fullscreen (borderless, the desktop's mode), the settings `NX2SARIA_WINDOW_SIZE`,
`NX2SARIA_FULLSCREEN` and `NX2SARIA_MONITOR`, the display's refresh rate, focus, the Vulkan surface. The swapchain and
the present are `gfx_present.c`'s. Keyboard and mouse events go to `input_sdl.c` and to the menu
(`runtime/ui/menu_sdl.cpp`).

SDL wants its video calls on the thread that initialised video, so this thread does them all: the others hand it work
through `run_on_window()` (a user event and a wait) or post a user event. Video is initialised before the window
exists: the Vulkan instance asks for SDL's surface extensions first (`gfx_window_vk_extensions()`), the window comes
with `gfx_window_start` (`wsys_open()`).

`NX2SARIA_WINDOW_SIZE` is in pixels. On a display that scales windows (Wayland, HiDPI) the window asks for its pixels
in the display's units (`SDL_GetWindowDisplayScale`), and SDL reports mouse positions in those units: `input_sdl.c` and
the menu scale them back (`gfx_window_sdl_density()`).

State: `g_ev_base` is what `SDL_RegisterEvents` gave for the user events (`EV_RUN`, `EV_SETTINGS`, `EV_CLOSE`,
`EV_TEXT_ON`, `EV_TEXT_OFF`, `EV_PASTE`); `g_video_ready` is a manual-reset event: SDL video is up (or failed);
`g_window_tid` is the window thread, which runs the video calls; `g_density_milli` the pixels per window unit, times
1000.

### `gfx_window_dpi()` `set_pixel_size()` `set_fullscreen()`

`gfx_window_dpi()`: the display scale as a DPI, 96 at 100% (the menu's text size floor); 96 before the window exists.
`set_pixel_size()` sets the window's size in its units for `w` x `h` pixels. `set_fullscreen()` uses the desktop's
mode: borderless over the display.

### `apply_settings()`

Window thread: the window takes the size and fullscreen state of the settings when they changed. A window resized by
hand keeps its size when another setting changes.

### `gfx_window_text_input()` `gfx_window_paste()`

SDL sends `SDL_EVENT_TEXT_INPUT` between `SDL_StartTextInput` and `SDL_StopTextInput` only (and shows an on-screen
keyboard there where the system has one, as Steam's on a handheld); the window thread makes the calls. SDL's clipboard
belongs to the window thread: the text is read there and handed to the menu.

### `report_placement()` `handle()`

`g_place_pending` is `NX2SARIA_MONITOR`'s display while a Wayland compositor has yet to place the fullscreen window
(`open_window()`): SDL learns the output a Wayland window is on later, with a display, scale or mode change (Hyprland
sends only the latter two).

In `handle()`, F11 does not act with a modifier held: overlays and capture tools take Ctrl, Shift or Alt+F11 for their
own. It writes `NX2SARIA_FULLSCREEN` and saves: the menu shows it, and the next start keeps it.

### `window_main()`

`SDL_HINT_APP_ID` is "nx2saria", Wayland's app id: the desktop file and the taskbar match it. SDL's Vulkan queries need
its Vulkan loader, which a window would load at creation: the instance and the device are made before the window, so
the loader is loaded here. After each event batch `input_sdl_idle` handles a cursor change requested from another
thread meanwhile.

### Monitor choice

`NX2SARIA_MONITOR`, `--monitor`, `--list-monitors`. The monitors come in `SDL_GetDisplays` order, which is the index
the setting counts in (1-based) and `--list-monitors` prints. The video driver sets that order, which need not match
the desktop's own list of monitors: it holds while the same monitors stay connected, but a monitor that appears or goes
away can move the ones after it. SDL has one name per monitor: make and model, with the connector (such as DP-2) on
Wayland and X11. The name pins one monitor more firmly than its index. These calls belong to the thread that
initialised video: the window thread, or the main thread in `--list-monitors`. `MON_MAX` is 32.

`MonInfo`: `rc` is the monitor's pixels, at its position in the desktop's coordinates; `dpi` the display scale as a
DPI, 96 at 100%; `name` is `SDL_GetDisplayName`, empty when SDL has none.

- `enum_monitors()`: a scaled desktop (Wayland at 125%) counts its coordinates in scaled units: the pixel density turns
  the size back into the monitor's pixels, and the content scale and the density together are the display scale
  (`SDL_GetWindowDisplayScale` gives a window the same product). The position stays in the desktop's units.
- `monitor_line()`: `2  "HP Inc. HP E243 CNK0161ZY2 (DP-3)"  1920x1080 at 3456,756  dpi 120 (125%)`: one monitor, for
  the log and `--list-monitors`, with "  primary" after the rectangle on the primary one.
- `resolve_monitor()`: which monitor of the list a `NX2SARIA_MONITOR` value (not empty, not auto) names, as a 0-based
  index; -1 when none. Rules, in this order: "primary"; all digits, a 1-based index; any other text, a
  case-insensitive substring of a monitor's name (the first monitor in list order when several match). `why` gets the
  rule that matched, or the reason nothing did. The same rules as `resolve_monitor()` of
  `runtime/gfx/gfx_window_win.c`, which also matches the GDI device name.
- `chosen_monitor()`: the monitor `NX2SARIA_MONITOR` picks, copied to `*out`; 0 when it is unset or auto (the system
  places the window), or when it matches nothing now (the monitors present are logged, and the default placement
  applies). `log`: say what was decided and why. The window thread asks with `log` set; the size query, which
  `widescreen.c` makes before the window exists, stays quiet. Window thread.
- `list_monitors()`: `--list-monitors` (`main.c`): one line per monitor, then what the current `NX2SARIA_MONITOR` would
  pick. No game files. It runs on the main thread before any window thread exists, so that thread initialises video
  here and shuts it down again.
- `gfx_window_monitor_size()`: the pixels of the monitor the window is on; 0 when unknown. Before the window exists
  (`wide_init` of `widescreen.c` at start) a fullscreen window opens on the monitor `NX2SARIA_MONITOR` picks, so that
  one's size; without a choice the system decides where the window goes, which is not known yet.

### `open_window()`

- `NX2SARIA_MONITOR`: the window is created centred on the chosen monitor; without a choice the system places it.
- `set_pixel_size()`: `w` x `h` were window units; the setting is in pixels.
- The window is centred again at the size it ended up with. SDL also takes this position as the display the fullscreen
  covers: a Wayland window cannot be moved, and this is how its fullscreen still opens on the chosen output.
- On Wayland a line says that the compositor places a windowed window, so `NX2SARIA_MONITOR` only decides the
  fullscreen one; `open_window()` runs once, so this is logged once.
- A Wayland compositor places the fullscreen window after `SDL_SyncWindow` returns; `handle()` reports it once the
  window is on the chosen monitor.

### `wsys_probe_click()`

The probe's synthetic events go through the event handler, on the window thread. SDL reports no activation by a title
bar click: only the client click exists.

## `runtime/gfx/gfx_window_win.c`

The Win32 back end of the window (`runtime/gfx/gfx_wsys.h`): the window and its message loop on a thread of their own,
F11 fullscreen, the settings `NX2SARIA_WINDOW_SIZE`, `NX2SARIA_FULLSCREEN` and `NX2SARIA_MONITOR`, the monitor's
refresh rate, focus, the Vulkan surface. The swapchain and the present are `gfx_present.c`'s. The in-game menu's
keyboard and mouse come from the window procedure (`runtime/ui/menu_win.cpp`), and so do the game's (`input_win.c`).
Only the Windows build compiles this file.

The process is per-monitor DPI aware (`nx2saria.manifest`): every size here is physical pixels, `NX2SARIA_WINDOW_SIZE`
included, and the swapchain matches the screen pixel for pixel. A window moved to a monitor at another display scale
keeps its client size (WM_GETDPISCALEDSIZE); the menu takes its text size floor from the window's DPI
(`gfx_window_dpi()`).

State: `g_wnd_ready` is a manual-reset event: the window thread made the window or gave up. `g_background` is
`NX2SARIA_BACKGROUND_WINDOW`: the window stays behind the user's windows. `g_fs_pending` (window thread):
`NX2SARIA_FULLSCREEN` did not apply before the first show and is retried at the first activation. `g_dpi` is the
window's DPI, set by the window thread. `g_saved_place` is the window behind fullscreen (window thread). The messages:
`WM_APP_FULLSCREEN` (WM_APP + 2), the fullscreen setting retried at the first activation; `WM_APP_TEXT_INPUT` (WM_APP +
3), the menu's text field takes text (wParam 1) or no longer (0); `WM_APP_PASTE` (WM_APP + 4), the menu's Paste: the
clipboard's text, read on the window thread.

### `outer_size_at()` `outer_size()` `fit_work_area()`

`outer_size_at()`: the outer size of a framed window whose client area is `cw` x `ch`, with the frame measured at
`dpi`. `outer_size()`: the same at the window's DPI, or the system DPI before the window exists: plain
`AdjustWindowRect` always takes the system DPI, which is wrong on a monitor at another display scale.

`fit_work_area()`: the client size `cw` x `ch` scaled down, in its own proportions, so the whole window fits a work
area: the chosen monitor's (`work`, with the frame measured at `dpi`) or, with `work` NULL, the primary monitor's,
where CW_USEDEFAULT opens it. The default 1920x1080 on a 1080p laptop put the bottom of the picture (the HUD) under the
taskbar and past the screen. The setting itself stays; the menu's size list offers what fits.

### `keep_awake()`

Windows turns the display off and sleeps after the idle timeout, and gamepad input does not count as activity. While
the window is up and not minimized it holds the display and the system on, as other games do. The state belongs to the
calling thread, so only the window thread (`window_main()` and `wnd_proc()`) calls this; the process exit releases it.

### `toggle_fullscreen()` `fullscreen_before_show()`

F11: a borderless window over the whole monitor, and back to the saved window. The swapchain follows through WM_SIZE.
Not Alt+Enter: Enter is the Plus button (`input_win.c`). A background window (test runs) keeps its place behind the
user's windows and takes no focus.

`fullscreen_before_show()` (window thread, before the window is first shown) gives the window the outer size `ow` x
`oh` (the windowed size F11 returns to) and then makes it fullscreen on the monitor it was created on, so the first
thing on screen is the fullscreen window. `toggle_fullscreen()` saves the placement of a window that was never visible,
which is its normal rectangle as just set. The window stays hidden here; the caller shows it. It returns whether the
window is fullscreen now. `showCmd` is set to SW_SHOWNORMAL: a hidden window may report SW_HIDE, which F11 would
restore as hidden.

### `apply_settings()`

`g_applied_size` and `g_applied_fs` are the window size and fullscreen state last taken from the settings (window
thread). `apply_settings()` acts on a change of either value only, so a window resized by hand keeps its size when
another setting changes. Window thread: the window takes the size and fullscreen state of the settings when they
changed. In fullscreen the size goes into the placement restored when fullscreen ends.

### `gfx_window_text_input()`

The menu's text field lives in this window, which has no IME (detached at creation): while the field takes text the
window gets the default one, so a player can type a name in Chinese, Japanese or Korean (Windows hands the composed
characters over as WM_CHAR, which `menu_window_msg` passes on), and it is detached again after. Posted to the window
thread, which owns the window's input context.

### `monitor_rate_hz()` `refresh_rate_update()`

The refresh rate of the window's monitor (`NX2SARIA_FPS_LIMIT=display`), measured when the window opens, lands on
another monitor or the display mode changes (`gfx_window_set_refresh` keeps it). `QueryDisplayConfig` gives the exact
rational rate (59.94, 143.998); `EnumDisplaySettings`' whole hertz is the fallback, where 0 and 1 mean the hardware
default. `refresh_rate_update()` runs on the window thread; `force`: the display mode changed, measure even on the same
monitor.

### Monitor choice

`NX2SARIA_MONITOR`, `--monitor`, `--list-monitors`. The monitors come in `EnumDisplayMonitors` order, which is the
index the setting counts in (1-based) and `--list-monitors` prints. The order follows the display driver's adapter and
output order, so it holds while the same monitors stay attached and enabled, but a monitor that appears or goes away (a
virtual display) can move the ones after it: the GDI device name or the monitor's name pins one monitor more firmly
than its index. `MON_MAX` is 32.

`MonInfo`: `rc` and `work` are the monitor's rectangle and its work area (without the taskbar), in desktop pixels;
`dpi` the effective DPI of the monitor, 96 at 100% display scale; `device` the GDI device name, such as
`\\.\DISPLAY3`; `name` the monitor's friendly name from the display configuration, empty when it has none.

- `monitor_dpi()`: `GetDpiForMonitor` lives in `shcore.dll`, which the build does not link; 0 when it is missing. The
  library is loaded once and kept: the monitor list asks for every monitor's DPI each time it is built. The DPI type
  passed is 0, MDT_EFFECTIVE_DPI.
- `fill_monitor_names()` fills `MonInfo.name`: an active display path ties a GDI device (the source) to the monitor
  attached to it (the target), and the target knows the monitor's name. Cloned monitors share a source, and the first
  path with a name wins. The query is tried 3 times: the topology can change between the size query and the query.
- `monitor_line()`: `2  \\.\DISPLAY3  "Name"  1920x1080 at 2560,0  primary  dpi 144 (150%)`: one monitor, for the log
  and `--list-monitors`.
- `contains_nocase()`: whether `text` contains `needle_lower`, which is already lower-cased; the comparison ignores
  case.
- `resolve_monitor()`: which monitor of the list a `NX2SARIA_MONITOR` value (not empty, not auto) names, as a 0-based
  index; -1 when none. Rules, in this order: "primary"; all digits, a 1-based index; any other text, a
  case-insensitive substring of a monitor's GDI device name or friendly name (the first monitor in list order when
  several match). `why` gets the rule that matched, or the reason nothing did.
- `chosen_monitor()`: as in the SDL back end: the monitor `NX2SARIA_MONITOR` picks, copied to `*out`; 0 when it is
  unset or auto (the system places the window), or when it matches nothing now (the monitors present are logged, and
  the default placement applies). `log`: say what was decided and why. The window thread asks with `log` set; the size
  query, which `widescreen.c` makes before the window exists, stays quiet.
- `list_monitors()`: `--list-monitors` (`main.c`): one line per monitor, then what the current `NX2SARIA_MONITOR` would
  pick. No game files.
- `center_in()`: the top-left that centres an `ow` x `oh` window in `work`; a window larger than the work area keeps
  its corner at the work area's corner, so its title bar stays reachable.
- `gfx_window_monitor_size()`: before the window exists (`wide_init` of `widescreen.c` at start), the monitor a
  fullscreen window opens on: the one `NX2SARIA_MONITOR` picks (`window_main()` places the window there), else the
  primary monitor.

### `exit_main()` `exit_start()`

The window's close runs on a thread of its own: `gfx_window_closed` saves the settings, the caches and the saves and
ends the process, which takes seconds while pipeline builds run (`vk_pipeline_save_exit` waits up to 5 s for them). On
the window thread the window stopped answering meanwhile; after 5 s Windows showed it as not responding, and an "End
task" there killed the process in the middle of its saves. The window is hidden as the end starts, so a window that
still answers does not invite a second close (one would find the end under way: `runtime/gfx/gfx_present.c`
`rt_exit_begin()`).

`exit_start()` returns the end's thread, NULL when the end had started already or runs here (no thread could be made).

### `wnd_proc()`

- WM_KEYDOWN, F11: bit 30 of lParam is auto-repeat. Not with Ctrl or Shift held: overlays and capture tools take such
  F11 shortcuts for their own (Alt+F11 comes as WM_SYSKEYDOWN). The setting is written and saved: the menu shows it,
  and the next start keeps it. `note_applied()` records what `cfg_get` reads now (the environment may still override
  it).
- WM_EXITSIZEMOVE: the window was dragged onto another monitor, so the refresh rate is measured.
- WM_DISPLAYCHANGE: a resolution, layout or HDR change of the desktop. A fullscreen window keeps covering the monitor
  at its new size (a same size sends no WM_SIZE). The swapchain is made again: its size, and its colour space after an
  HDR switch.
- WM_GETDPISCALEDSIZE: a framed window keeps its client size in pixels on a monitor at another scale. For a fullscreen
  or maximized one `DefWindowProc` runs: Windows scales the size with the DPI.
- WM_DPICHANGED: the display scale changed, or the window reached a monitor at another scale. A fullscreen window keeps
  covering the monitor the suggested rectangle lies on.
- WM_ACTIVATEAPP: the end's own hide (`exit_start()`, which sets `g_exit_started` first on this thread) deactivates the
  window: no player's focus loss. Taken as one, the present thread's next present wrote the session's report for the
  whole run and started a new period (`perf_focus_lost` in `perf.c`), so "frametime: exit" covered only the frames
  since.
- WM_ENDSESSION: Windows is shutting down, restarting or logging off: it ends the process once this message returns,
  without a WM_CLOSE, so the close's end runs (the settings menu's pending save, RTSS's profile, a save Commit in
  flight) and this thread waits for it here; the end does not return. When a close had started the end already, the
  thread sleeps for good.

### `raise_window()`

Brings the game window to the front. Windows refuses `SetForegroundWindow` to a process that was not the last to get
input, which is the case for a game started by a frontend; attaching this thread's input queue to the foreground
window's thread for the call is the documented way around it. The window is not left topmost. If it is still refused, a
topmost toggle puts it above the others without keeping it there.

### `in_tri()` `pointer_cursor()`

The window's pointer: a gold arrowhead in the menu's colours, its tip the hotspot at the top left, the half towards
the tip's right edge a lighter facet, with a dark outline; sized for the system DPI (32 px at 96 DPI) and drawn with
4x4 samples per pixel into a 32-bit cursor with alpha. The arrow when anything fails. The arrowhead: the tip (ax, ay),
the right wing (bx, by), the lower wing (cx, cy), the notch (mx, my) between them. The pixels are premultiplied by
alpha, as 32-bit cursors want.

### `window_main()`

- The window class takes icon resource 1 of `nx2saria.rc` (`runtime/nx2saria.ico`): the title bar, the taskbar button
  and Alt+Tab.
- `NX2SARIA_MONITOR`: the window is created centred in the chosen monitor's work area, with its frame measured at that
  monitor's DPI, so it opens there at the scale it will have. Without a choice the system places it. Either way the
  size is scaled down first if it does not fit the work area it opens in.
- The window opens behind the user's windows first and is then raised (`raise_window()`): a game started by a frontend
  has no other way to reach it. The game counts itself in focus until the first WM_ACTIVATEAPP, so loading goes on, and
  keyboard input waits for the foreground (`input_win.c`). If Windows still refuses the foreground, the taskbar button
  flashes. With `NX2SARIA_FULLSCREEN` the window is made fullscreen on the monitor it was created on before it is first
  shown (`fullscreen_before_show()`), so no framed window appears; the windowed rectangle stays in `g_saved_place` for
  F11. If that fails, `g_fs_pending` retries at the first activation (`wnd_proc()`, WM_ACTIVATEAPP).
  `NX2SARIA_BACKGROUND_WINDOW=1` (test runs): no flashing, never fullscreen at start, and F11 keeps the window's place.
- An IME switched on over the game attaches to this window and then shows its candidate window and eats the keys it
  composes, although there is nothing here to type into (a Chinese IME takes Shift, the B button by default). Detaching
  the game's window (`ImmAssociateContext` with NULL) leaves the software keyboard alone: its edit box sits in a window
  of its own (`runtime/hle/swkbd_win.c`) and gets the IME it needs. The menu's text field attaches one while it takes
  text (`gfx_window_text_input()`).
- The frame was measured at the system DPI (or the chosen monitor's); the window may have opened on a monitor at
  another scale. When the size changes on a chosen monitor, the window is centred again in its work area; otherwise it
  keeps the place it was created at.
- The fullscreen setting: the window turns fullscreen on the monitor it is on before it is shown, and keeps that place
  and size; the windowed size stays for F11.
- `g_fs_pending` is set before the raise: the activation the raise gets is the first WM_ACTIVATEAPP, which takes the
  fullscreen setting; set after it, fullscreen waited for the next activation and the game started in a window. Only a
  window that did not become fullscreen before it was shown still needs that retry. The raise matters because a
  frontend's player may have no way to reach a window left behind.

### `wsys_open()` `wsys_surface()`

`wsys_open()` makes the window on its thread and waits until it is there. `wsys_surface()` creates the surface through
Streamline when DLSS Frame Generation is loaded: it maps swapchains to windows from this call.

## `runtime/gfx/resolution.c`

The game resolution: the size the game composes its frame in (its output: canvas, UI and swapchain) and the size of
its 3D scene, above the shipped 1920x1080 and 1600x900. The game does the rendering itself: the patch sets `output` and
`scene` (`recomp/patches-v121.txt`, `recomp/patches-v143.txt`) make the game read both sizes from the patch page, and
the texture heap that holds the output- and scene-sized render targets grows with them. The licence notice in its
fifth and sixth lines stays in the code. What the player sets and what it costs:
[presets.md](../presets.md#rendering-the-3d-scene-below-the-pictures-size).

Settings, read at start: `NX2SARIA_OUTPUT_RES = <w>x<h>` (default 1920x1080, the game's own; taken within 1280x720 to
7680x4320, each side made even) and `NX2SARIA_RENDER_SCALE`, the scene's size relative to the output: "game" (the
default, 5/6, the game's 1600x900 at 1080p) or a percentage (25 to 400); above 100 the game supersamples its scene,
which smooths edges (anti-aliasing) at the cost of GPU time. The game's own final pass (FSR 1 EASU) scales the scene to
the output; the present scales the output to the window (`runtime/vk/vk_fsr.c`).

The patch page:

| Offset | Holds |
|---|---|
| +16 | the scene size {w, h} as floats |
| +32 | the output size as {w, h} in one 64-bit word |
| +40 | the renderer (1.2.1) or the display configuration (1.4.3) the patched code stores there |
| +48 | the swapchain size as {w, h} in one 64-bit word |
| +56 | the camera's aspect ratio as a float when the output is not 16:9 (set `aspect`) |
| +60 | the field-of-view factor (set `fov`, `res_fov_live()`) |
| +92 | the fixed sun shadow map size (set `shadow`) |

1.4.3 scales the scene it reads by output width / 1920 (a 2560x1440 output renders its 1600x900 scene at 2128x1200),
so its page holds the wanted size divided by that factor and the game's 5/6 needs no scene patch. 1.2.1 renders the
size it reads (1600x900 on a 2560x1440 output): its page holds the wanted size, and the scene set is on whenever the
output differs from 1920x1080.

The heap: agl's DynamicTextureAllocator is one fixed heap, "DynamicTextureAllocatorMem" in
`System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs` (113664 KiB, `DTA_KIB`, carved from "Graphics", 201728 KiB). At
2560x1440 the shipped size runs out at the title screen. Both heaps grow by the same amount: the shipped size times
the output's pixel ratio above 1920x1080 plus the scene's above 1600x900 (measured: a 3440x1440 canvas with a
4620x1935 scene crashed at +258 MiB and loaded at +400 MiB; this formula gives +750 MiB there). `runtime/hle/romfs.c`
serves the rewritten table (`res_heapdef_build()`) in place of the original; `runtime/memory.c` adds the amount to the
application pool, so no other heap shrinks. Without the rewritten table (a RomFS that lacks it, a format this code does
not know) the patch sets stay off and the game keeps its own sizes.

`SCENE_MAX` (4096) is per side. `SHADOW_BASE` (960) is the sun shadow map's base size (its cascades: a D16 array of 4
layers).

The fields of `R`: `ow`, `oh`, `sw`, `sh` the output and the scene; `scene_set`, the scene differs from the game's 5/6
of the output; `heapdef_ok`, `romfs.c` serves the grown HeapDef; `on`, `res_init()` switched the patch sets on;
`shadow`, the fixed sun shadow map size (`NX2SARIA_SHADOW_RES`, 256 to 4096 in multiples of 8), 0 the game's own;
`dlss_k`, for `NX2SARIA_DLSS` quality, balanced, performance or ultra_performance, the scene's share of the output,
else 0; `base_k`, the scene's share without a DLSS mode (render_scale, or the game's 5/6); `hw`, `hh`, the largest
scene a DLSS mode change can ask for (the heap is sized for it); `live`, 1.4.3 with DLSS able to run: the DLSS mode can
change while the game runs.

### `dlss_dll()`

The scene size is decided before the device exists, so NGX cannot be asked yet. NGX loads DLSS from `nvngx_dlss.dll`
next to `nx2saria.exe` on Windows, `libnvidia-ngx-dlss.so.<version>` beside the game executable on Linux
(`vk_dlaa_init` in `runtime/vk/vk_dlaa.c`): without that file DLSS never runs, and a DLSS mode would leave the game's
plain scaling at 50..67 % of the output.

### `scene_for()` `dlss_share()`

`scene_for()`: the scene size for a share `k` of the output, within `SCENE_MAX` per side.

`dlss_share()`: the scene's share of the output for a `NX2SARIA_DLSS` mode (quality 2/3, balanced 0.58, performance
0.5, ultra_performance 1/3); 0 for dlaa (the scene keeps `base_k`), while DLSS/DLAA is off (the mode the menu still
holds would shrink the scene for the plain upscaler) and where DLSS cannot run (`dlss_dll()`). Ocarina's builds have no
scene patch set: its DLSS render scale is `widescreen.c`'s (the `dlss_share` there), and the driver's NGX supplies the
model where the file is missing, so there is nothing to report for it.

### `decide()`

A DLSS mode other than dlaa sets the scene size instead of the render scale. On 1.4.3 with DLSS able to run (DLAA on,
the DLL there) the mode can change while the game runs (`res_dlss_mode()`): the scene patch is then always on and the
heap is sized for the largest scene the modes ask for. Everyone else keeps the game's own scene size and its dynamic
resolution unless render_scale or a DLSS mode asks for another, and a heap sized for that scene alone.

### `shadow_kib()` `res_adjustable()` `res_heap_extra_kib()`

`shadow_kib()`: the texture heap for a sun shadow map above the base size: the four layers of 2 bytes, three maps in
flight (the engine makes the array anew each frame).

`res_adjustable()`: the generated C carries the output and scene patch sets for the builds that have them. Without
them (Ocarina) the game draws at its own sizes whatever these settings say; its heap grows for its widescreen picture
instead (`wide_heap_extra_kib` of `widescreen.c`), which is what `res_heap_extra_kib()` returns there. Otherwise the
amount is rounded to whole MiB.

### `key_name()` `field()` `res_heapdef_build()`

The HeapDef table is BYML version 2..7, little endian ("YB"): a hash node is {u8 0xC1, u24 count} and count entries
{u24 key index, u8 type, u32 value}; an int (type 0xD1) is stored in the entry, so a size changes in place.

`key_name()` checks its offsets in `size_t`: a mod's table offset near 2^32 would wrap a 32-bit sum below `n`.
`field()`: the offset of the value of hash node `node`'s entry `name`, which must have type `type`; 0 when there is
none.

### `shadow_init()`

The sun shadow map size: each frame the engine sizes its shadow cascades to the area they cover, down to about 400
near the ground, which draws foliage shadows blocky, and each change of size moves the shadow texels, so leaf shadows
jump. A fixed size (the `shadow` set, `recomp/patches-v121.txt`) keeps them still and sharper. Page +92.

### `res_shadow_range()`

`NX2SARIA_SHADOW_RANGE` (percent, 50 to 400; 100 = the game's): the sun's shadow cascades end at 20, 40, 80 and 160 m.
Other values multiply those distances through the `cascade` patch set (`recomp/patches-v143.txt`), so the sharp first
cascade reaches further and the visible steps between cascades move away from the player, at the cost of shadow texels
per metre (`NX2SARIA_SHADOW_SCALE` pays that back).

### `res_init()`

- `fs_init()` first: `romfs.c` serves the grown HeapDef from there on (`res_heapdef_served()`). Sizes at or below the
  shipped ones fit the shipped heaps; a larger one without the grown table leaves the game its own resolution.
- `k` is 1.4.3's own scaling of the scene with the output width.
- With `R.live`, `g_fnprof` is set until the game sets its render size: `res_fn_hit()`, or `res_present()` gives up.
- Another shape than 16:9 (more than 1% off): the camera takes the output's ratio (`recomp/patches-v121.txt` and
  `recomp/patches-v143.txt`, set `aspect`), so a 21:9 output shows more of the world at the sides instead of a
  stretched 16:9 view. The HUD's layouts for that shape come from `romfs-patches-<w>x<h>` (`overlay_init` in
  `runtime/hle/romfs.c`, `tools/make_romfs_patches.py --aspect`). At 16:9 the set is switched off: the page holds no
  ratio.

### `res_dlss_lod_bias()`

DLSS upscaling: textures are sampled for the output's resolution, not the scene's (NVIDIA's DLSS guide: a mip bias of
log2(render / output), and -1 more with the jitter, which `dlss_mode` in `runtime/vk/vk_dlaa.c` adds), so their detail
survives the scaling. 0 without a DLSS mode. Without the output and scene patch sets (Ocarina) the scene's share is
`widescreen.c`'s (`wide_dlss_share`: the mode's share of the picture, 1 for DLAA, 0 off), which follows the mode and
the upscaler while the game runs; `R.dlss_k` stays the mode at start there (`res_dlss_mode()` moves it only once
`res_init()` has switched the sets on). `dlss_mode` asks again at every mode change.

### `res_is_16x9()` `res_wants_wide_overlay()`

`res_is_16x9()`: 16:9 within 1% (1366x768 counts): the shape the game's own layouts and camera are made for.
`res_wants_wide_overlay()`: the game resolution asks for a `romfs-patches-<w>x<h>` overlay: it is not 16:9 and the game
version has the camera aspect patch (set `aspect`); the UI layouts of another shape only fit a camera that follows the
same shape.

### `res_fov_factor()` `res_fov_live()`

Field of view (`NX2SARIA_FOV`, percent of the game's own, 70..140, `FOV_MIN` and `FOV_MAX`, live): the camera's
vertical view angle times the factor. The 1.1.1 callback reads it here (`res_fov_factor()`) and 1.4.3 reads it from
page +60. The horizontal angle follows through the aspect ratio. 100 switches the set off. `tests/fov_check.py` reads
the log line of `res_fov_live()` from this file.

### `res_fn_hit()` `res_present()` `res_dlss_mode()`

A `NX2SARIA_DLSS` change while the game runs (`runtime/vk/vk_dlaa.c`, at a present): the new scene size goes to the
patch page, where the game reads it when it sets its render size (`main+0x585e48`). 1.4.3 does that once, at start: its
renderer setup (at 0x7000e67028) calls `main+0x585bd0`(renderer, force), which picks the console mode's sizes and flags
the render targets for re-creation (`SET_RENDER_SIZE`, 0x7000585bd0). The setup call is caught through the
function-entry hook (`res_fn_hit()`, on only until then), and the next nvnQueuePresentTexture on the game's thread
calls it again with force set (`res_present()`), between frames as at start. The heap was sized for the largest scene
a mode asks for. `res_dlss_mode()` returns 1 when the size changed.

`renderer` is x0 of the game's own call of `SET_RENDER_SIZE`; `size_pending` says the page holds a size the game has
not set yet.

In `res_present()`: the hook runs on every guest function entry while it waits: should the game set its render size
before `res_init()` or never, it stops after 1800 presents (half a minute at 60 fps) and modes change at the next
start. The guest's registers are kept as at the present call and put back: the setup runs as if it never happened.

## `runtime/gfx/mesh_fb.h`

The mesh shader fallback's modules (`runtime/gfx/mesh_fb.c`): what a task or mesh program computes, as compute modules
that write it into storage buffers, and the vertex stage that draws from those buffers. No Vulkan calls here. Where the
fallback sits: the bullet "Mesh fallback" in [architecture.md](../architecture.md).

`MESHFB_MAX_LINES` (136) is 8 system value components and 32 generic vectors of 4.

`MeshFbInfo` is a task or mesh program's stage buffer, as `MeshInfo` of `tools/xlate/AmpereLowering.cs` lays it out in
the shared memory of the translated module:

| Field | Meaning |
|---|---|
| `stage` | 6 task, 7 mesh |
| `threads` | the workgroup size |
| `maxv`, `maxp` | mesh: the vertex and primitive limits |
| `ipp` | mesh: indices per primitive (1 points, 2 lines, 3 triangles) |
| `payload_bytes` | the task payload (task, or a mesh program a task feeds), with its header |
| `has_task` | mesh: a task stage feeds it |
| `nlines` | mesh: output components per vertex |
| `lines` | their attribute addresses: 0x6c point size, 0x70.. position, 0x80 + 16 g + 4 c |
| `attr`, `map`, `pay`, `shared_bytes` | byte offsets in shared memory, and its size |

- `MESHFB_CB_TASK` (0x41), `MESHFB_CB_MESH` (0x42) and `MESHFB_CB_LIST` (0x43): where a module's extra storage buffers
  are bound: the `.res.json` "cb_slot" of their entries (`ssbo_info` in `runtime/vk/vk_exec_draw.c`).
- `MESHFB_LIST_TASK_BITS` (24): a list entry holds the task workgroup in bits 0-23 and the mesh workgroup's index among
  that task's in bits 24-31.
- `meshfb_info()`: from the program's control section and the first 0x80 bytes of its code (the header with the output
  map). 0 with `err` filled when the program uses what the fallback does not carry.
- `meshfb_slot_words()`: the words one mesh workgroup's output takes in the mesh buffer: its vertex lines and its
  primitive area.
- `meshfb_task_cs()`: the compute module of a TaskEXT module (`spirv`, as the translator wrote it): the same program,
  then the payload of workgroup t into words [t * payload_bytes / 4, ..) of the task buffer, a storage buffer at
  `binding`. malloc'd; NULL with `err` filled.
- `meshfb_mesh_cs()`: the compute module of a MeshEXT module. Workgroup g writes its stage buffer
  (`meshfb_slot_words()`) into slot g of the mesh buffer at `binding` + 1. After a task stage (`m->has_task`) entry g
  of the list at `binding` + 2 names its task workgroup and its index among that task's mesh workgroups, and the
  payload comes from the task buffer at `binding`, `task_payload_words` apart.
- `meshfb_vs()`: the vertex module that draws the mesh buffer (binding 1): instance = slot, `maxp * ipp` vertices an
  instance; vertex index / ipp is the primitive, and a primitive past the slot's count is degenerate. Outputs:
  position, point size for points, and a 4-component vector at every Location of `out_mask`, of `out_type[location]`
  (`GFX_TYPE_*`).
- `meshfb_res_json()`: a copy of the translator's `.res.json` text `res` for a compute module made above: its stage is
  "cs" and its storage buffers gain the entries of the extra buffers (`n` of them from `binding` on, `cb_slots[i]`
  each). malloc'd.

## `runtime/gfx/mesh_fb.c`

The mesh shader fallback's modules, for a GPU without VK_EXT_mesh_shader (the "mesh fallback" draws of
`runtime/vk/vk_exec_draw.c` use them).

The translator turns a task or mesh program into a compute module whose shared memory is the program's stage buffer
and wraps it in a TaskEXT or MeshEXT entry function (`tools/xlate/MeshSpirv.cs`). The wrapping is undone here and
replaced: the translator's main function stays as it is, the entry function that hands the stage buffer to the mesh
pipeline goes, and a compute entry function copies the stage buffer into a storage buffer instead.

- task: main; the payload area into the task buffer at the workgroup's place.
- mesh: [the payload from the task buffer into the payload area] main; the vertex lines and the primitive area into the
  workgroup's slot of the mesh buffer.
- draw: a vertex module made here reads a slot per instance: vertex index / indices per primitive is the primitive, its
  index byte names the vertex, and the vertex's lines are the outputs.

The module is made from the translation the shader cache already holds, so the translator and with it every cache key
stay as they are: a card with mesh shaders never sees these modules, and one without needs no other cache.

Mesh workgroups after a task stage: a task workgroup starts up to `threads` mesh workgroups, and how many is known
only on the GPU. A pass between the two dispatches (`runtime/shaders/meshfb_scan.comp`) lists them in task order: entry
g of the list names the task workgroup and the index the mesh program sees as its workgroup id. That id comes from the
WorkgroupId built-in in the translated code; here the built-in counts list entries, so main reads a private variable
the entry function fills from the list.

Opcode and enum numbers are the SPIR-V 1.6 specification's and SPV_EXT_mesh_shader's.

### `meshfb_info()`

The payload as the translator sizes it (`Translate` in `tools/xlate/Program.cs`): a size of 0 is one 256-byte unit, and
the header of 0x20 bytes comes on top.

The output map of the program header: system values B at byte 0x35, one bit per component from 0x60; generic vectors
from byte 0x36, a nibble each. System values A and C never appear in the game's mesh programs (bytes 0x34, 0x48 and
0x49 must be 0).

### The word list and the emit helpers

`Cx` is what a module under construction adds: decorations, then types, constants and variables, then one function;
`t_void`, `t_bool`, `t_uint`, `t_fnvoid` and `t_v3uint` are existing ids or new ones.

- `K()`: a 32-bit unsigned constant. `E_()`: an instruction with a result.
- `if_begin()` and `if_end()`: `if (cond) { ...the instructions emitted until if_end }`.
- `loop_begin()` and `loop_end()`: `for (i = start; i < limit; i += step) { ... }`; `counter` is a Function variable.
- `word_buffer()`: a storage buffer of 32-bit words at (set 0, `binding`): its variable. `readonly`: the module only
  reads it.

### `convert()`

A translated task or mesh module to its compute module. `In` is an instruction: its first word's index, opcode and
word count. `is_decl()` says an opcode is a type, constant or global variable declaration.

- Pass 1 finds what the module has. The shared memory array must hold the layout this module is asked to copy. Then
  the old entry function, and the function it calls: the translator's main.
- New types, variables and the entry function. For a mesh module, the workgroup id main sees is a private variable (see
  above). `first` marks the workgroup's first thread. The barrier is OpControlBarrier with execution scope 2
  (Workgroup), memory scope 2 and semantics 0x108 (AcquireRelease | WorkgroupMemory).
- Task: the mesh workgroup count starts at 0: a task that culls everything never writes it.
- Mesh: the whole stage buffer starts at 0: the vertex lines, the primitive count and the vertex indices. Workgroup
  memory is undefined at the start, a mesh program need not write every vertex it has room for or every index byte (nor
  its count when it emits nothing), and the slot is copied whole and drawn: what an earlier workgroup left there became
  vertices and primitives. The payload area after it is copied in next. A slot without primitives gets its count alone.
- Pass 2 writes the module again without the old entry function, the MeshShading capability and the
  SPV_EXT_mesh_shader extension. In the OpEntryPoint every global variable is its interface (SPIR-V 1.4); the name is
  "main" (0x6e69616d). In a mesh module, main and what it calls read the workgroup id from the private variable.

### `meshfb_vs()`

- The types are kept by `GFX_TYPE_*`: float, sint, uint.
- A point list (`ipp` 1) needs its size written.
- The constant 0xc1800000 is -16: outside the clip volume, for a point of no primitive.
- A slot past the buffer (more workgroups than it has room for) draws nothing.
- Byte `map + 4 + ipp * prim + corner` of the slot is the vertex.
- A vertex's line is word `((v >> 5) * lines + line) * 32 + (v & 31)` of the vertex area.
- Lines below 0x6c (layer and viewport index) are not carried, nor an output nothing reads.
- A component outside the program's output map reads as Maxwell gives an unwritten one: (0, 0, 0, 1), the w as the
  bits of 1.0f whatever the type, as `spirv_add_outputs` in `runtime/vk/vk_pipeline.c` pads a missing output.
- The memory model is Logical, GLSL450 (0, 1); the entry point is "main" (0x6e69616d).

## `runtime/gfx/mesh_fb_hold.h`

The mesh fallback's depth hold ("a list's depth-only draw is held" in `runtime/vk/vk_exec_draw.c`): which lists'
colour draws were skipped lately. Header only, so that `tests/mesh_fb_hold_check.c` checks the table without a device.

A list is its indirect record's address in the game's memory (below 2^39). One word per slot: the address above
`MFB_HOLD_BITS` (24) bits of the present count. The colour draw stamps its list with the present it was skipped in and
clears the stamp when it is drawn; the list is held while the stamp is at most `MFB_HOLD_PRESENTS` (2) presents old.
Read and written by the replay's threads without a lock: a lost update costs a list one frame of the hold, and two
lists in one slot (`MFB_HOLD_SLOTS`, 1024, of them, by a hash of the address) hold only the one stamped last.

- `mfb_hold_mark()`: the list's colour draw was skipped for its modules or pipeline (`waits` 1) at this present, or was
  drawn (0).
- `mfb_hold_test()`: whether the list's depth-only draw is held at this present.

## `runtime/gfx/mfb_batch.h`

The mesh fallback's batch (`mfb_draw` in `runtime/vk/vk_exec_draw.c`, `NX2SARIA_MESH_FALLBACK_BATCH`): the rule, apart
from Vulkan so that `tests/mfb_batch_check.c` runs it.

A mesh draw through the fallback runs its program's stages as compute dispatches that write the fallback's buffers,
and an ordinary draw reads them. One by one, each draw ends its pass for its dispatches (a full barrier between every
two steps) and the draw after it begins the pass again. Batched, the draws of one pass have their dispatches in the
part's pre-pass command buffer (`vk_sync_pre`), which the queue runs before the part, and the pass stays whole: every
draw of the batch needs a part of the buffers of its own, since all the dispatches run before all the draws.

A draw's part (`mfb_batch_need()`):

- its mesh workgroups: the count itself for a draw the CPU knows the count of and that has no task stage; else
  `MFB_HEADROOM` times the most the program asked for in a draw so far (its history, read back from the records, from
  `MFB_MIN_SEEN` of them), and at least a floor: what 1/`MFB_FLOOR_DIV` of the buffer holds (`mfb_floor()`). A history
  of a few records undersizes: in Kokiri Forest programs that had asked for 32 workgroups at most asked for 72, 81 and
  454 a moment later (a batch at twice the history, 2026-11-22);
- its task workgroups, with a task stage: the count itself for a direct draw, else the same rule.

A draw goes one by one, as before (the way back), when (`mfb_batch_need()`, `mfb_batch_steps()`): batching is off; one
of its stages writes memory (a later draw of the pass could read it); its program once asked for more than its part
held in a batch (excluded for the session); its program has no history yet where the part would come from it; its task
or mesh stage reads a texture or image (its bind may record an image's first fill, which needs the part's pass); its
part does not fit even a pre-pass command buffer of its own; there is no pre-pass command buffer, or the part could not
be cut.

The cut (`mfb_batch_steps()`): a batched draw cuts the part, so that its stages go to a new pre-pass command buffer
that runs after everything the part recorded before, when

- the pass the pre-pass command buffer serves ended since (`mfb_need_cut()`);
- a draw went one by one since the parts began (`closed` in `runtime/vk/vk_mesh_fb.c`): it wrote the buffers from their
  start, over the parts. A draw one by one ends its pass, so this is the case above; the flag keeps it so if that
  changes. Until 2026-11-22 every later draw went one by one instead (`MFB_ONE_CLOSED`), which lost about half the
  batching in Kokiri Forest;
- its part does not fit what the parts left: they start over in the new pre-pass command buffer.

A task overflow: `runtime/shaders/meshfb_clamp.comp` keeps the task workgroups asked for in the record's report word
(13) before it cuts the dispatch to the part's room, so a batched draw short of its task part excludes its program like
one short of its mesh part (until 2026-11-22 the scan read the count already cut, and such a draw was never seen).

The constants:

| Name | Value | Meaning |
|---|---|---|
| `MFB_MIN_SLOTS` | 64 | the floor's own floor, for programs whose workgroups are large |
| `MFB_MIN_TASKS` | 16 | the same for task workgroups |
| `MFB_MIN_SEEN` | 8 | records read back before a part comes from the history |
| `MFB_MIN_CALM` | 32 | and records in a row since its last jump (`mfb_hist_note()`): the history has settled |
| `MFB_READMIT` | 256 | records in a row without a jump that take an excluded program back into batches |
| `MFB_MAX_SHORTS` | 3 | the batched draws short of their part after which a program stays one by one |
| `MFB_HEADROOM` | 4 | the factor on the history |
| `MFB_FLOOR_DIV` | 32 | a part holds at least 1/32 of a buffer: 32 such parts fit a pre-pass command buffer (a longer run of draws cuts for room). At 1/64 (483 workgroups of 3.8 KB) one draw of a program with a settled history of 120 asked for 577 in every Kokiri Forest start |

The reasons a draw goes one by one:

| Reason | Meaning |
|---|---|
| `MFB_ONE_OFF` | `NX2SARIA_MESH_FALLBACK_BATCH=0` |
| `MFB_ONE_WRITES` | a stage writes memory |
| `MFB_ONE_HISTORY` | too little history for a part the GPU sizes |
| `MFB_ONE_EXCLUDED` | the program overflowed its part once |
| `MFB_ONE_ROOM` | the part does not fit a pre-pass command buffer of its own |
| `MFB_ONE_PRE` | no pre-pass command buffer, or the part could not be cut |
| `MFB_ONE_TEXTURES` | a task or mesh stage reads a texture or image (`mfb_draw`: its bind may record an image's first fill or an alias copy, `vk_exec_inline_begin`, which needs the part's pass) |
| `MFB_ONE_LOADING` | an area's textures are loading (`vk_exec_pool_loading`): its frames are not shown, and the programs' histories have not settled yet (every short part seen on the bench was there) |

`MfbHist` is a program's history (`MfbCs.hist`), updated where `runtime/vk/vk_mesh_fb.c` reads a record back
(`record_check`). The parts of a program whose draws GPU-written counts size come from its history. In the bench's
sessions every batched draw short of its part came in the first seconds (the loading after the start, before Link
arrived), from a history of a few records still growing: parts of 141 to 483 workgroups for draws of 498 to 1581
(2026-11-22). A history is taken only once it has settled: `MFB_MIN_CALM` records in a row without a jump (a record
more than twice the most so far, or the first). A batched draw short of its part excludes the program (its draws one
by one, which draw whole); `MFB_READMIT` calm records later, with the history now holding what it asked for, it is
batched again, and after `MFB_MAX_SHORTS` such draws it stays one by one for the session. Its fields: `slots` and
`tasks`, the most mesh and task workgroups one of its draws asked for; `seen`, its records read back; `excluded`, a
batched draw of it asked for more than its part held (until it is taken back); `calm`, records in a row since the last
jump; `shorts`, its batched draws short of their part, this session.

- `mfb_floor()`: the workgroups of 1/`MFB_FLOOR_DIV` of a buffer of `cap` bytes, each of `unit` bytes; at least
  `least`.
- `mfb_twice()`: `MFB_HEADROOM` times `v`, at least `least` (the name says twice; the factor is 4).
- `mfb_batch_need()`: the workgroups a batched draw's part holds, or a reason it goes one by one. `direct`: the CPU
  knows `count`. `floor_slots` and `floor_tasks` are the parts' floors (`mfb_floor()`). With a task stage, the mesh
  workgroups the tasks start are known only on the GPU.
- `mfb_part_bytes()`: the bytes of a part of `slots` mesh and `tasks` task workgroups: the task payloads, the list (a
  word per mesh workgroup, with a task stage only) and the mesh outputs (`slot_words` per mesh workgroup).
- `MfbSpan` and `MfbPart`: the part's buffers, filled from the start for each pre-pass command buffer
  (`mfb_part_reset` at its end).
- `mfb_need_cut()`: whether a batched draw cuts the part first (`vk_exec_split_now`): unless the pass the pre-pass
  command buffer serves (`pre_pass`, set after the cut's draw began it) is still the open one. `pass_serial` goes up
  only where a pass begins, so a pass ended since (a uniform update's copy, a game dispatch, a draw one by one) leaves
  it equal: what the part recorded after that end would run after the pre-pass command buffer that the draw's stages
  go to.
- `mfb_parts_taken()`: parts taken since the buffers' use started over: a draw one by one then closes them (a cut
  before the next batched one).
- `mfb_part_take()`: the next part of `task_b`, `list_b` and `mesh_b` bytes (each rounded up to `align`, the storage
  offset alignment) from `used` within `cap`; 0 when one does not fit (`used` is left as it was). A part of 0 bytes
  still takes `align`, so its binding has a size.
- `mfb_hist_note()` (`MFB_NOTE_SHORT` 1, `MFB_NOTE_READMIT` 2): what a record read back says (`record_check`): the
  program's history grows, and a batched draw that asked for more than its part held excludes the program from batches.
  It returns `MFB_NOTE_SHORT` when that draw was short, `MFB_NOTE_READMIT` when the program is taken back into batches,
  else 0. A draw one by one that asked for more than the whole buffers hold (a garbage count the GPU wrote,
  `meshfb_clamp.comp`) raises the history only to what they held: a part from it fits them still (else every later draw
  of the program would be one by one, or cut for nothing). The draws of the program already in flight come back short
  together: one exclusion counts once.
- `MfbSteps` and `mfb_batch_steps()`: `mfb_draw`'s steps after `mfb_batch_need()`, with the cut, the pre-pass command
  buffer and the part as calls (`vk_exec_split_now`, `vk_sync_pre`, `vk_meshfb_part_take`), so that
  `tests/mfb_batch_check.c` runs the same order. `cut`: the part ends here, 0 when it could not be cut; `pre`: the
  pre-pass command buffer, 0 none; `take`: the draw's part of the buffers, 0 when it does not fit what is left.
  `*cut` is `MFB_CUT_NONE`, `MFB_CUT_PASS` (at the pass or closed) or `MFB_CUT_ROOM` (for room). `why` is
  `mfb_batch_need()`'s answer; `images`, a task or mesh stage reads a texture or image; `need_cut`, `mfb_need_cut()`,
  or a draw went one by one since the parts began. It returns `MFB_BATCHED` (the part taken) or why the draw goes one
  by one; `*cut` says whether and why the part was cut on the way (a cut for a draw then one by one was for nothing).
  When the part does not fit right after a cut, the buffers' use started over with that cut: the part is too large for
  them.
- `MfbCount` and `mfb_count()`: the counts of the log line (`mfb_batch_line` in `runtime/vk/vk_exec_draw.c`). `ok`: a
  batched draw's stages were recorded; otherwise it was batched and its stages' pipeline failed: the draw was skipped.

## `runtime/gfx/mesh_emu_spirv.h`

Mesh shader emulation for GPUs without VK_EXT_mesh_shader (`NX2SARIA_MESH_EMULATION`, `runtime/vk/vk_pipeline.c` and
`runtime/vk/vk_exec_draw.c`): the translator's TaskEXT and MeshEXT modules (`tools/xlate/MeshSpirv.cs`) become
GLCompute modules that write their vertices, triangles and task payloads into one GPU buffer (the arena), and a
generated vertex shader reads the vertices back for the game's fragment shader. Plain C over SPIR-V words; no Vulkan
types.

The arena is one buffer, its address in the push constants of every emulation stage:

- words 0-3: vertex slots used, indices reserved, task records reserved, overflows (the replay zeroes them per
  submission);
- `MESH_EMU_DOFF` (256): draw records (`MESH_EMU_DRECS`, 4096 of them), `MESH_EMU_DWORDS` (16) words each:
  DrawIndexedIndirect {indexCount, instanceCount, firstIndex, vertexOffset, firstInstance}, the mesh dispatch {x, y,
  z}, the task base, then the `MESH_EMU_R_*` words; the replay fills them, and the allocation shader
  (`mesh_emu_alloc_spirv()`) reserves each draw's task records and index range, so that the stages of many draws can
  run together: a draw's indices are one range (its DrawIndexedIndirect) whatever the order its mesh workgroups run in;
- `MESH_EMU_TOFF`: task records, `MESH_EMU_TWORDS` words each: the mesh workgroup count, then the payload;
- the index area (uint32 indices) and the vertex area (16-byte slots: position, then each generic output), at offsets
  the replay passes in the push constants.

| Word | Value | Meaning |
|---|---|---|
| `MESH_EMU_R_GROUPS` | 9 | mesh workgroups with output: the task stage adds the counts it emits |
| `MESH_EMU_R_FIRST` | 10 | the first stage's workgroups (the replay: the count, or a copy of the indirect one) |
| `MESH_EMU_R_ICUR` | 11 | indices taken from the draw's range (the mesh stage's atomics) |
| `MESH_EMU_R_ICAP` | 12 | the draw's range: indices reserved (they fit in the area) |
| `MESH_EMU_R_PER` | 13 | indices one mesh workgroup may emit (OutputPrimitives x vertices per primitive) |
| `MESH_EMU_R_TASK` | 14 | 1 with a task stage |

`MESH_EMU_TWORDS` (780) is 1 + payloads of up to 776 words (3072 bytes and the 32 the translator adds for the payload
header since 0.6.0), padded. There are as many task records as the push constants say (the replay sizes them,
`NX2SARIA_MESH_EMU_TASKS`); the index area follows them.

The push constants of every emulation stage (compute and the vertex shader), 32 bytes (`MESH_EMU_PC_BYTES`): u64 arena
address; uint draw record (word index); uint index area (word index); uint index capacity; uint vertex area (16-byte
slot index); uint vertex capacity (slots); uint task record capacity. Header words 0-2 count what the stages asked
for, beyond the capacities too (the replay logs the most per submission); word 3 counts mesh workgroups that did not
fit.

- `mesh_emu_alloc_spirv()`: the allocation shader (GLCompute, one invocation): for the `n` draw records from ring
  index `first`, mode 0 reserves the task records of draws with a task stage (`MESH_EMU_R_FIRST` of them, from word 2)
  and the index ranges of draws without one (`MESH_EMU_R_FIRST` x `MESH_EMU_R_PER`, from word 1); mode 1, after the
  task stages, the index ranges of draws with one (`MESH_EMU_R_GROUPS` x `MESH_EMU_R_PER`). Push constants, 32 bytes:
  u64 arena address; uint first, n, mode, index capacity, task record capacity, unused. Static words, `*n` of them.
- `MeshEmuInfo`: `task` 1 TaskEXT, 0 MeshEXT; `threads` LocalSize x; `max_v`, `max_p` (mesh) OutputVertices and
  OutputPrimitivesEXT; `prim` (mesh) 3 triangles, 2 lines, 1 points (triangles and points are emulated); `psize_slot`
  (mesh) the slot holding the point size (x), 0 without one; `payload_words` the task payload's size (mesh: what it
  reads, 0 without a task stage); `max_mesh` (task) the most mesh workgroups one task workgroup launches (the dispatch
  width); `slots` (mesh) 16-byte slots per vertex: position and each generic output; `n_generic` (mesh) generic
  outputs, in slot order 1..n; `generic_loc` their locations.
- `mesh_emu_compute()`: the compute module for a TaskEXT or MeshEXT module (malloc'd, `*out_n` words); NULL with `why`
  set when the module is not one the converter knows (it then stays unemulated: its draws are skipped as before).
- `mesh_emu_prim()`: a task or mesh module's output primitive: 3 triangles, 2 lines, 1 points, 0 not a mesh module.
- `mesh_emu_vs()`: the vertex shader of an emulated mesh pipeline: position and the generic outputs from the vertex
  slots, as `types[l]` (0 float, 1 signed, 2 unsigned) with `comps[l]` components for every location l in `need` (the
  fragment stage's inputs); a location the mesh stage does not write is zero. malloc'd, `*out_n` words. `fsw` is the
  fragment shader's SPIR-V; its input variables are matched one for one (Location, Component, type, Flat), else (NULL,
  or a declaration the parser does not take) one vector per location of `need`.

## `runtime/gfx/mesh_emu_spirv.c`

Mesh shader emulation: the SPIR-V rewriting. The input is what `tools/xlate/MeshSpirv.cs` makes: the translator's
compute main, called from an entry function that copies its shared-memory stage buffer into the mesh outputs after
SetMeshOutputsEXT (mesh), or into the task payload before EmitMeshTasksEXT (task).

- **Mesh to GLCompute:** the output arrays and the task payload move to Workgroup memory; SetMeshOutputsEXT is dropped
  (its counts are kept); at the entry function's end thread 0 reserves vertex slots and indices in the arena with
  atomics, and every thread copies vertices and triangles out (indices made absolute vertex slots, relative to the
  vertex area). With a task stage mesh workgroup (x, y) is mesh group x of task record (task base + y): a prologue
  returns when x is past the record's count, loads the payload, and the module's WorkgroupId reads become (x, 0, 0).
- **Task to GLCompute:** the payload moves to Workgroup memory; EmitMeshTasksEXT becomes a store of the payload and the
  mesh group count into the task record, and workgroup 0 writes the mesh dispatch {max_mesh, task workgroups, 1} into
  the draw record.

### The emit helpers

- `word_load()` and `word_store()` use the memory operand Aligned (2) with 4.
- `atomic()`: Device scope (1), relaxed semantics (0).
- `barrier()`: OpControlBarrier with Workgroup (2), Workgroup (2) and 264 (AcquireRelease | WorkgroupMemory).
- `OutVar.role`: 0 position, 1 generic, 2 indices, 3 point size.

### `mesh_epilogue()`

The mesh epilogue replaces the entry function's return.

- Header word 6 counts the mesh workgroups that reached the output copy; word 7 the primitives they declared (for
  diagnosis).
- Indices come from the draw's own range (reserved by the allocation shader), vertex slots from the arena's. The index
  base is made relative to the area, as firstIndex. An atomic add puts the count into the draw record's index count.
- An output of role 3 is the point size, written in x.

### `task_epilogue()`

The task epilogue replaces EmitMeshTasksEXT x y z.

- An atomic max into header word 2 counts the records asked for, past the capacity too.
- The payload is copied in rounds of the workgroup's threads; the last round's threads past the payload idle.
- The mesh group count is added to `MESH_EMU_R_GROUPS` of the draw record, for the index range.
- Task records past the area are not written: their mesh groups are not dispatched (the subtraction is opcode 130,
  OpISub).

### `mesh_emu_compute()`

- It first finds the output variables and the payload, then makes the new types, constants and variables.
- Output and payload pointers become Workgroup pointers: in place when no such pointer exists yet, else mapped (a
  mapped one is marked dropped with the storage class 0xffffffff, the other rewritten in place).
- Then one pass over the module: types, constants and global variables, then the function code. In the check for an
  unexpected WorkgroupId use only id operands are looked at: literals of these instructions may equal the id (OpExtInst
  word 4; opcodes 81 and 82 and 79, OpCompositeExtract, OpCompositeInsert and OpVectorShuffle, from their literal
  words; OpLoad from word 4, OpStore from word 3; 251, OpSwitch; OpSelectionMerge; 246, OpLoopMerge).
- In the entry function the function variables stay first.
- Header words 4 and 5 count the mesh workgroups started and past the prologue's check (diagnosis: `mesh.txt` of
  `vk_dump`). The prologue's payload copy runs as `task_epilogue()`'s.
- Assembling: the magic 0x07230203; the version is raised to 0x00010500, SPIR-V 1.5, where physical storage buffers are
  core; the memory model is PhysicalStorageBuffer64 (5348), GLSL450 (1). The entry point lists every global variable;
  the new ones are found by scanning the new types for OpVariable.

### `fs_inputs()` `mesh_emu_vs()`

The vertex shader of an emulated mesh pipeline. `fs_inputs()` reads the fragment shader's input variables as declared:
Location, Component (decoration 31), scalar type and width, Flat (decoration 14). 0 when one is not a plain 32-bit
scalar or vector (the caller then goes by the reflection's per-location summary).

In `mesh_emu_vs()`:

- Without declared inputs the per-location summary gives one vector per location.
- `vecs[type][comps]` are the vector ids (comps 2..4), `vecs[type][1]` the scalar.
- Outputs: one per fragment input, with the same Location, Component and type; Flat when the input is, or when the
  type is an integer one.
- The code: slot = vertex area + VertexIndex; each output comes from slot + its index. Slot 0 is the position (entry 32
  of `slot_of`); the loop starts at -1 for the position.
- An output the mesh stage does not write is zero.
- A narrower output is cut out with OpVectorShuffle (79): components comp..comp+k-1.
- The module header is 0x07230203, 0x00010500.

### `mesh_emu_alloc_spirv()`

The allocation shader, as static words (`alloc_words`): compiled with `glslangValidator -V -g0 --target-env vulkan1.2`,
then `spirv-opt -O --strip-debug`, clean under `spirv-val`. A static assertion ties its constants to the header's
(`MESH_EMU_DOFF / 4 == 64`, `MESH_EMU_DRECS == 4096`, `MESH_EMU_DWORDS == 16`, `MESH_EMU_R_GROUPS == 9` and the others).
Its source:

```glsl
#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 1) in;
layout(buffer_reference, std430, buffer_reference_align = 4) buffer Words { uint w[]; };
layout(push_constant) uniform PC { uvec2 addr; uint first; uint n; uint mode; uint icap; uint trecs; uint pad; } pc;
void main() {
    Words a = Words(pc.addr);
    for (uint d = 0u; d < pc.n; d++) {
        uint r = 64u + ((pc.first + d) % 4096u) * 16u;
        bool task = a.w[r + 14u] != 0u;
        if (pc.mode == 0u && task) {
            uint tb = a.w[2], k = a.w[r + 10u];
            a.w[r + 8u] = tb;
            a.w[2] = k > pc.trecs ? pc.trecs + tb : tb + k;
        } else if ((pc.mode == 0u) != task) {
            uint g = pc.mode == 0u ? a.w[r + 10u] : a.w[r + 9u], per = a.w[r + 13u], ib = a.w[1];
            uint ni = per != 0u && g > pc.icap / per ? pc.icap + 1u : g * per;
            a.w[r + 2u] = ib;
            a.w[r + 12u] = ib >= pc.icap ? 0u : min(ni, pc.icap - ib);
            a.w[1] = min(ib + ni, 0x7fffffffu);
        }
    }
}
```

## `runtime/gfx/mesh_pipeline_check.h`

Reports a mesh draw's cache-key check without changing the draw; -1 means the cache entry was not found.
`mesh_pipeline_check_start()` writes the start line: the check runs on the mesh emulation's draws only
(`mesh_prepared_check` in `runtime/vk/vk_exec_draw.c`), and a run without a report line says nothing otherwise.

## `runtime/gfx/mesh_fb_probe.c`

`nx2saria.exe --probe-meshfb`: the mesh shader fallback (`runtime/gfx/mesh_fb.c`, `runtime/vk/vk_mesh_fb.c`, the "mesh
fallback" of `runtime/vk/vk_exec_draw.c`).

The draw part needs no game files: two mesh programs of the runtime's own in the shape the translator gives one
(`runtime/shaders/meshfb_probe_*.mesh` and `.task`), one without a task stage and one fed by a task program, drawn
through the replay into an RGBA8 target with direct and with indirect workgroup counts, and both in one pass after each
other. Each picture is drawn twice, through VK_EXT_mesh_shader and through the fallback, and the two are compared texel
by texel; without mesh shaders on the device only the fallback draws, and the probe says that nothing was compared.

The module part runs with `NX2SARIA_PROBE_MESHFB_DIR=<dir>`: every "<name>" of `<dir>/list.txt` names `<dir>/<name>.in`
(a stored shader input, `in_store` of `gfx_shader.c`: control section and code) and `<dir>/<name>.spv` (its
translation). The compute module and, for a mesh program, a vertex module are written to `<dir>/out/<name>.cs.spv` and
`.vs.spv` for spirv-val (work the caller does; tests of game shaders stay outside the repository). Where
`<dir>/<name>.res.json` (the translation's reflection) is there too, the compute pipeline of the module is built on
this device, as its first draw would build it.

`vk_exec_probe_shader()` and `vk_exec_mfb_batch_counts()` are in `runtime/vk/vk_exec_draw.c`; the second fills the
counts of `MfbCount` (`runtime/gfx/mfb_batch.h`).

### `modules_part()`

- `h` is the stored input's header: magic, version, stage, code length, c1 length, size of the answers (`InHeader` of
  `gfx_shader.c`). `GFX_STAGE_TS` and `GFX_STAGE_MS` are the NVN stage numbers 6 and 7.
- The vertex module is asked for with output types `g % 3`, so every output type gets made somewhere.
- With a `.res.json`, the pipeline is built with the layout the reflection gives; the first pool brings the device up.
- At the end an empty submit: the pool's buffers go when a submission after their release has completed.

### `fake_stage()` `make_prog()`

`fake_stage()` makes an NVN2 control section of a task (6), mesh (7) or fragment (1) stage as `vk_exec_draw.c` and
`mesh_fb.c` read it, and a code block whose header maps the outputs: position and generic vector 0. Its words: +0x58
is 32 threads and a payload of one 256-byte unit (`32 | 1 << 24`); +0x5c triangles (`3 << 24`, with `1 << 16` when a
task feeds it); +0x60 64 vertices and 32 primitives (`64 | 32 << 16`). `make_prog()`: the program of a mesh stage, its
task stage if any, and the fragment stage.

### `draw_picture()` `drawn_texels()` `differing()`

`draw_picture()`: the uniform data is in the pool, 0x1000 apart per draw: the mesh stage's block at 0x90000, the task
stage's at 0x98000; the indirect records at 0xa0000. `drawn_texels()` counts texels unlike texel 0, which is in the
margin no cell covers. `differing()`: the texels of `a` and `b` that differ, and the largest difference of a channel.

### `draw_part()`

- The cases: 24 cells of a 6 x 4 grid by 24 mesh workgroups; 40 cells of an 8 x 5 grid by 10 task workgroups that
  start 0 to 4 mesh workgroups each.
- `SHORT()` counts the draws that did not fit: one by one (`vk_meshfb_short`) and batched short of their part
  (`vk_meshfb_batch_short`; `vk_meshfb_short` reads every record back first).
- More workgroups than the mesh buffer has slots for (the slots are the buffer's words / 540, and the draw asks for
  1000 more): what fits is drawn, the rest is dropped and counted. The cells past the grid are outside the picture, so
  it is the 24-cell picture again.
- A GPU-written task count past what the task buffer holds (its bytes / 4 + 1000, past the room of any payload size):
  the clamp cuts the dispatch and keeps the count asked for in the record (`meshfb_clamp.comp`), so the draw is
  counted; until 2026-11-22 the scan read the count already cut and such a draw was never seen. The tasks past the 10
  of the grid draw outside the picture.
- As the game runs it: pipelines built in the background, and a program's modules made on the fallback's own thread.
  New shader objects of the same programs are drawn until the picture is whole: the draws before that are skipped, and
  none may leave a partial picture behind for good.
- With `NX2SARIA_MESH_FALLBACK_BATCH=1` the pictures were drawn through `mfb_draw`'s batch and its way back (the
  pictures are the same as the mesh pipeline's and as one by one, checked before): the draws batched, those one by one
  for want of history (a task program's first draws), and the cuts at each pass and after a draw one by one. Then more
  draws of programs with history, so that task draws are batched too. A batched draw's stages use compute pipelines of
  their own, built in the background: the draws before they are ready are skipped, as above, and once the picture is
  whole it stays whole.

### `probe_meshfb()`

No game is loaded: Link's own motion vectors (`motion_owner.c`, selective_motion) would read the player's actor in
guest memory at every submission (`vk_usnap_begin`) and fault. The probe draws no game, so
`NX2SARIA_SELECTIVE_MOTION` is set to 0.

## `runtime/gfx/spirv_water.h`

The water corrections of [water-projective.md](../water-projective.md) as edits of a SPIR-V module in memory
(`runtime/gfx/spirv_water.c`); the same edits `tools/projective_recipe.py` makes on files.
`runtime/vk/vk_pipeline.c` applies them to the exact translations it knows.

Each returns a new module (free it) with its word count in `*out_n`, or NULL when the module is not of the shape the
edit expects; the caller then keeps the original.

- `spirv_water_vertex()`: the location 0 output gets (clip x, clip y, clip w, 1) of the position the stage wrote,
  before its return.
- `spirv_water_fragment()`: the location 0 input loses NoPerspective and is read through a local holding (x / w * 0.5 +
  0.5, y / w * -0.5 + 0.5, 0, 1), which is what the vertex stage wrote there before, now divided per fragment.
- `spirv_water_absorption()`: the store of id `distance` through id `pointer` stores NMax(distance, 0) instead.

## `runtime/gfx/spirv_water.c`

The water corrections, made on a SPIR-V module in memory. Each edit follows `tools/projective_recipe.py` step for
step, new ids and declarations in the same order, so the module it returns is the one that tool writes for the same
input (`tests/water_projective_check.py` compares them on a made-up module).

A module is taken apart into its instructions, edited as a list, and put together again. New types and constants are
collected and placed before the module's one function; an existing type or constant of the same definition is used
instead of a new one. Every edit refuses a module that is not of the shape it was written for: one entry point, one
function, one return, and the interface or the store it looks for found exactly once.

- `Ins`: `n` is the words after the first, `a` those words. `Mod`: `extra` holds the new types and constants, in the
  order made (up to 24); `src` is the module's words, and the instructions' arguments point into this copy; `pool`
  (256 words) the arguments of the instructions added; `bad`, an edit ran out of room and the module is refused.
- `mod_parse()` leaves room for 64 more instructions: what the edits insert. `insert()` relies on it.
- `type_of()`: the type or variable instruction that defines `id`, among the module's own and the new ones.
- `type_id()`: the id of the type (`op`, `v[0..k)`), an existing one or a new one. `constant_id()`: the id of the 32-bit
  constant `bits` of type `ty`, an existing one or a new one.
- `location0()`: the one interface variable of the storage class at location 0, a four-component 32-bit float vector.
- `glsl_import()`: the id of the GLSL.std.450 import when the module has exactly one.
- `mod_finish()`: the edited module's words, the new declarations placed before the function; it frees `m`.
- `spirv_water_vertex()` looks for the OpMemberDecorate BuiltIn with value 0, Position.
- `spirv_water_fragment()`: the NoPerspective decoration goes; the stage's own reads of the input go to the local; the
  new code is placed after the function's own variables.
- `spirv_water_absorption()`: NMax(distance, 0): a negative distance through the water must not turn the dimming into a
  gain.

## `runtime/gfx/spv_store.h`

A translated shader's SPIR-V that is kept zstd-compressed while nobody reads it ("SPIR-V at rest" in
`runtime/gfx/gfx_shader.c`).

A store holds the SPIR-V as a plain copy, as a zstd frame, or as both. Readers pin it (`spv_pin()`), which hands out
the plain copy and decodes the frame first when the plain copy is gone, and unpin it when done (`spv_unpin()`). One
thread, the packer, makes the frame of a store that has gone unused for a while and then frees the plain copy
(`spv_pack_*`).

Lock rules, which make a reader never see a buffer swapped or freed under it:

- every store of one owner shares one lock (`PlatMutex`), held only for a few loads and stores: never across a decode,
  an encode or a free, so a reader on the drawing thread waits for no codec;
- `plain` changes under the lock only: a reader sets it from NULL to a buffer it decoded, the packer sets it back to
  NULL, and only while `pins` is 0. A reader gets the pointer and counts its pin in the same locked section, so the
  copy it holds cannot be freed before its unpin;
- `z` is set once, by the packer, before any plain copy is dropped, and stays until `spv_store_free()`: a reader that
  found `plain` NULL under the lock decodes `z` outside it;
- one packer per owner at a time (the owner's own lock around its passes): `spv_pack_begin()` and `spv_pack_end()` of
  one store pair up, and nothing else drops plain copies.

Two readers that find the frame alone both decode it; the first one to come back installs its copy and the other frees
its own. The frame is checked by decoding it before the plain copy may go (`spv_pack_encode()`), so a store never holds
a frame that does not give back its SPIR-V bit for bit.

Header-only so that `tests/spv_store_check.c` runs it against the platform layer and libzstd alone.

- `SpvStore`: `plain`, the SPIR-V readers get, NULL while the frame alone holds it (changes under the lock); `words`,
  its length in 32-bit words, fixed once the store is filled; `z`, its zstd frame, exactly `zsize` bytes, set once and
  freed by `spv_store_free()` only; `pins`, readers between `spv_pin()` and `spv_unpin()` (under the lock); `used_ms`,
  when the last reader let go, or when the store was filled (under the lock); `fnv`, FNV-1a (64-bit) over the SPIR-V
  bytes once `fnv_ok` is 1.
- `SpvPacker`: the packer's buffers, kept from one store to the next: the encoder's output before it is copied out at
  its size, and the decode that checks it.
- `spv_store_fill()`: the store takes `plain` (malloc'd, `words` long) as its plain copy, last used at `now_ms`.
- `spv_pin()`: the plain SPIR-V, valid until the matching `spv_unpin()`. `*decode_ns`: the time spent decoding the
  frame here, 0 when the plain copy was there. NULL only when the decode could not allocate its buffer or the frame did
  not decode to the store's length (`spv_pack_encode()` checked it, so neither should happen). When another reader
  decoded it meanwhile, its copy is used and this one goes.
- `spv_fnv()`: the FNV-1a of the SPIR-V: worked out at the first call (pinning the store, so decoding it if it is
  compressed) and kept. `spv_pack_encode()` works it out before it compresses, so asking a compressed store decodes
  nothing. Racing callers store the same value.
- `spv_idle_locked()`: whether the store has gone unused for `idle_ms` at `now_ms`: no pin, and its last use at least
  that long ago (a use stamped after the packer read the clock counts as recent). The caller holds the lock.
- `spv_pack_begin()`: the packer's first step on a store: `SPV_PACK_NONE` when there is nothing to do (no plain copy,
  in use or used within `idle_ms`); `SPV_PACK_DROP` when the frame exists and the plain copy may go (`spv_pack_end()`);
  `SPV_PACK_ENCODE` when the frame is to be made first: the plain copy is pinned for the packer, which encodes it
  (`spv_pack_encode()`) and then calls `spv_pack_end()` in any case.
- `spv_pack_encode()` encodes the plain copy the packer pinned (`spv_pack_begin()` gave `SPV_PACK_ENCODE`) at zstd
  level `level` and checks the frame by decoding it. No lock is held: readers go on using the plain copy meanwhile
  (pinned by `spv_pack_begin()`, it is neither swapped nor freed until `spv_pack_end()`). It returns the frame
  (malloc'd, `*zsize` bytes) or NULL when the encoder failed or the check found a difference; the FNV is worked out
  here too.
- `spv_pack_end()`: the packer's last step on a store. After `SPV_PACK_ENCODE`, `z` is what `spv_pack_encode()` gave
  (NULL: the store stays plain) and the packer's pin goes (with no use stamp: it is not a reader); then the plain copy
  is freed if the store still has the frame and is still idle (a reader may have come meanwhile). It returns the bytes
  freed. A frame another packer step installed first is freed (that does not happen with one packer per owner).

## `runtime/gfx/prp_file.h`

The layout of `shader-prepared.bin`, the list of shader variants a preparation translated and built ahead of play
(`runtime/gfx/gfx_shader.c` `gfx_shader_prepare_seed()` writes it; `gfx_shader_warm_list()` and `lw_start` and
`lb_warm_prepared` of `runtime/vk/vk_pipeline.c` read it; `prepared_from` in `tools/make_game.py` reads its head).

- "NX2NSPRP" (`PRP_MAGIC`), version (u32, `PRP_VERSION` 2), record size (u32, 36, `PRP_REC`): `PRP_HEAD`, 16 bytes.
- Version 2 only: what the list was prepared from, 96 bytes (`PRP_FROM_SIZE`, `PrpFrom`): the sha256 of
  `seed-programs.bin`, the sha256 of `seed-answers.bin` (each over the whole file), and the translator identity (the 32
  bytes of `nx2navi_xlate.id`, before the host options are mixed in).
- Records: stage, priority, 2 zero bytes, input id (32 bytes); in priority order, one per input id.

A version 1 file has no such block: it does not say what it was prepared from. The seed decides which variants are in
the list, and nothing else recorded it: a build that came with another seed (more shaders listed, another pipeline
list) kept the old list, and the variants only the new seed names were translated when play first drew them. With the
block the build compares it with the seed it has just written and prepares again when they differ (`step_shaders` in
`tools/make_game.py`). The translator identity is recorded for the same comparison; a list prepared by another
translator is not discarded for that alone, since input ids do not depend on the translator and the game carries the
translations over (the carry of `gfx_shader.c`). `tests/prp_file_check.py` checks this header against the reader in
`tools/make_game.py`.

- `prp_head_write()`: the head of a version 2 file into `out` (`PRP_HEAD` + `PRP_FROM_SIZE` bytes); it returns that
  size.
- `prp_parse()`: the records of the file `f` of `size` bytes: a pointer into `f` and their count in `*n`; NULL for a
  file that is no prepared list or has no whole record. `*has_from`: 1 with `*from` filled for a version 2 file, 0 for
  a version 1 file.
- `gfx_shader_prepared_read()` is declared here and defined in `gfx_shader.c`.

## `runtime/gfx/tex.h`

CPU-side texture code: block-linear to linear copies and back, the ASTC decode, the NVN format to VkFormat mapping.
Vulkan memory and the texture objects are in `runtime/vk/vk_memory.c` and `runtime/vk/vk_texture.c`.

From `runtime/gfx/tex_deswizzle.c`:

- `nvn_deswizzle()` and `nvn_swizzle()`: block-linear to linear and back for one level and layer of a texture. The
  linear side is laid out by block rows: `dst_pitch` bytes per row (at least the row bytes of the level), compressed
  formats one block row per row, 3D textures slice k at `dst + k * dst_pitch * block rows`. LINEAR and
  LINEAR_RENDER_TARGET storage is copied by pitch. Multisampled and BUFFER textures return -1; success returns 0.
- `nvn_deswizzle_mt()`: `nvn_deswizzle` with a large level's block-linear copy split over the `par_jobs` pool
  (`tex_bl_copy_mt()`): the same bytes. For the texture upload, which runs on a drawing thread; the caller keeps the
  storage mapped until it returns.
- `nvn_texels_box()`: one box of a level and layer, [x, x + wb) bytes by [y, y + h) block rows of slice z (3D
  textures), between the storage and caller rows of `pitch` bytes, touching nothing else of the level: ReadTexels and
  WriteTexels of a region cost the region, not the level. The same textures as `nvn_deswizzle`; -1 for others and for a
  box outside the level.
- `nvn_bl_offset()`: the byte offset of (x byte, y block row, z slice) inside one level of block height 2^th and block
  depth 2^td GOBs.
- `nvn_deswizzle_geom()`: the geometry of a level as the two functions above use it; -1 for textures they reject.
- `tex_bl_copy()`: the copy loop on its own, for callers that know the level geometry (`bl` points at the level's first
  byte). `tex_bl_copy_mt()`: the same with a level of 1 MiB or more split into bands of rows over the `par_jobs` pool;
  the same bytes.

From `runtime/gfx/tex_astc.c` and `runtime/gfx/tex_astc_mt.c`:

- `astc_decode_ldr()`: ASTC LDR 2D to RGBA8, bit-exact with astcenc's 8-bit decode. `blocks`: row-major 16-byte blocks
  covering ceil(width/bw) x ceil(height/bh); `rgba_out`: width*height*4 bytes. `srgb` only selects the sRGB endpoint
  expansion. Error blocks and HDR content decode to magenta.
- `astc_block_opaque()`: whether the block decodes to alpha 255 in every texel; mostly from its header, blocks of the
  endpoint modes with alpha by a decode (counted in `*decoded`).
- `astc_blocks_opaque()`: the first of the `n` blocks (16 bytes each) that is not opaque, or that took `*decoded` past
  `max_decoded`; `n` when none is. `dst` (optional): all `n` blocks are copied there too, as by memcpy, the rest also
  after a block that is not opaque.
- `astc_blocks_opaque_mt()` (`tex_astc_mt.c`): `astc_blocks_opaque` with the blocks of a large texture spread over the
  decode workers, each also copying its part to `dst` when given; whether every block is opaque (and `*decoded` stayed
  within `max_decoded`, summed over the workers).
- `astc_decode_ldr_mt()` (`tex_astc_mt.c`): the same bytes as `astc_decode_ldr`, large images split by block rows over
  a worker pool.

From `runtime/vk/vk_formats.c`, the fields of `TexVkFormat`:

| Field | Meaning |
|---|---|
| `vk` | the format of the image created on this GPU |
| `ideal` | the format the NVN format maps to by itself; differs from `vk` after a fallback |
| `need` | optimal-tiling features `vk` must report for sampling and upload |
| `expand_rgb` | a 3-component format: the upload expands each texel to 4 components (alpha, or integer 1) |
| `alpha_one` | an "X" format: sample alpha as 1 (view swizzle) |
| `chan` | nonzero `chan[0]`: `vk` stands in with another component order; `chan[i]` is the NVN_TEXTURE_SWIZZLE_R..A selector of the image component holding R, G, B, A |
| `astc_bw`, `astc_bh` | nonzero: ASTC decoded into `vk` (RGBA8, same colour space) by `vk_astc.c` or `tex_astc.c`; zero for native ASTC (`vk == ideal`, raw blocks as for BC) |
| `astc_bc` | ASTC decoded and then encoded on the GPU (`vk_astc.c`): 1 BC3 (`NX2SARIA_ASTC_BC`), 2 BC1 for a texture whose every block decodes opaque ("ASTC as BC1" in `vk_texture.c`) |
| `depth`, `stencil` | aspect flags of depth and stencil formats |
| `note` | why a fallback or an expansion was chosen; NULL when the mapping is direct |
| `reason` | unmapped formats only: why there is no Vulkan format |

- `tex_vk_format()`: 1 when `nvn_format` (an NVNformat value) has a Vulkan mapping, 0 otherwise (`out->reason` says
  why). The mapping is fixed per device: ASTC decodes to RGBA8 unless `vk_astc_native` (`NX2SARIA_ASTC`) takes the
  format as it is, D24S8 is D32S8, DEPTH24 is D32. Whether the GPU reports the features is a separate question
  (`tex_vk_format_supported()`).
- `tex_vk_format_supported()`: 1 when `vk_ctx()->phys` reports every bit of `f->need` for `f->vk` in optimal tiling. It
  needs `vk_init()`.

## `runtime/gfx/tex_astc.c`

The ASTC LDR decoder, 2D only (Khronos Data Format specification, the ASTC chapter). It matches ARM astcenc's 8-bit
decode bit for bit (the `decode_unorm8` path its command line uses for `-dl` and `-ds` into 8-bit images): the
endpoints expand to 16 bits (linear: v * 257; sRGB: v << 8 | 0x80, all four channels), a texel is
`(e0 * (64 - w) + e1 * w + 32) >> 6`, and the output is its top 8 bits. Error blocks (illegal encodings, HDR void
extents) give magenta; HDR endpoint modes give magenta endpoints for that partition only (as astcenc and the
specification's LDR profile do).

The file's first line is a licence notice and stays in the code. The file is also compiled outside the game build, by
`launcher/gameart.py`, `tools/ui_textures.py`, `tools/layout_textures.py` and `tools/menu_preview.py`, so it has to keep
compiling alone.

The tables:

- `q_bits`, `q_trits` and `q_quints`: the quantization levels QUANT_2 .. QUANT_256 (index 0..20): bits, trits and
  quints per level.
- `wunq_data` and `cunq_data`: the unquantization tables indexed by the raw ISE value (trit or quint << bits | bits),
  generated from the specification's formulas. Weights: QUANT_2..QUANT_32 give 0..64. Colours: QUANT_6..QUANT_256 give
  0..255. The rows of `cunq_data` are, in order, those of 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160 and
  192 levels (`cunq_off`: 0, 6, 14, 24, 36, 52, 72, 96, 128, 168, 216, 280, 360, 456, 584, 744); QUANT_256 is the
  identity and has no row. The two tables are 142 and 936 bytes (a static assertion).

### `rbits()` `ise_decode()` `part_of()`

`rbits()`: n (at most 32) bits at `pos`; bits past 127 read as zero. `ise_decode()` decodes `count` raw ISE values (tq
<< bits | bits) starting at bit `pos`. In `part_of()` z is 0 for 2D, so seeds 9..12 drop out.

### `unpack_endpoints()`

`e0[0..3]` is endpoint 0 and `e1[0..3]` endpoint 1, 8-bit. It returns
false for an HDR mode. The modes:

| Mode | Meaning |
|---|---|
| 0 | L direct |
| 1 | L delta |
| 4 | LA direct |
| 5 | LA delta (an arithmetic shift of the signed deltas) |
| 6, 10 | RGB scale; 10 with two alphas |
| 8, 12 | RGB(A) direct; blue contraction: swap and uncontract |
| 9, 13 | RGB(A) base + delta; a negative sum: swap and uncontract |
| 2, 3, 7, 11, 14, 15 | HDR |

### `astc_decode_block()`

- A footprint below 4 or above 12 a side is not an ASTC 2D footprint.
- A void extent (`(mode & 0x1FF) == 0x1FC`): with bit 0x200 it is an HDR (fp16) constant, an error in LDR; the reserved
  bits (10 and 11) must be 3. The colour is the top 8 bits of the UNORM16s (bytes 9, 11, 13, 15).
- The weights are read backwards from the top of the block. Then the colour endpoint modes; the endpoints, expanded to
  16 bits; the weight grid to texel weights (bilinear infill), per axis.
- The interpolation shifts by 14: `>> 6`, then the top 8 bits.

### `block_opaque()` `astc_block_opaque()`

Whether `astc_decode_block()` gives every texel of the block alpha 255. The header tells for a void extent (its
constant's alpha), an error block (magenta, alpha 255) and colour endpoint modes without alpha (0 to 3, 6 to 9, 11, 14
and 15; the HDR ones decode to magenta in this LDR decoder). The modes with alpha (4, 5, 10, 12, 13; `cem_alpha()`) can
still decode to 255 everywhere, so such a block is decoded and counted in `*decoded`. The CEM fields are read as in
`astc_decode_block()`; a block whose one mode has no alpha is opaque whether its block mode is legal or an error, so
only partitions of different modes need the weight bits that place the rest of their CEM field. With one partition, or
partitions that share a mode, there is one mode for the whole block. An error found while decoding fills magenta, alpha
255.

### `quick_opaque()` `astc_blocks_opaque()`

The checks run 4 KiB at a time (`OPAQUE_RUN`, 256 blocks), each run copied to `dst` after its check, while the bytes
are in the cache: the copy costs what memcpy would, and the check little more than the read memcpy makes anyway.

`quick_opaque()`: the first 32 bits of a block settle almost every block: not a void extent, and one endpoint mode for
the whole block (one partition, or partitions that share it) that carries no alpha (`CEM_ALPHA_MASK`: bits 4, 5, 10, 12
and 13). The others go through `block_opaque()`. The word is read little-endian, as `load64()` reads it.

### `astc_decode_ldr()`

A block is decoded in place when its rows are contiguous in the output.

## `runtime/gfx/tex_astc_mt.c`

`astc_decode_ldr_mt()`: `astc_decode_ldr` with the block rows of one image spread over a persistent worker pool. Each
block goes through `astc_decode_block` and lands where `astc_decode_ldr` puts it, so the output is the same bytes. The
caller takes part and returns when every row is done. One image at a time: a second caller while the pool is busy, or
an image under `MT_MIN_BLOCKS`, decodes on its own thread.

`astc_blocks_opaque_mt()`: the same pool runs the opaque check of a large texture ("ASTC as BC1" in
`runtime/vk/vk_texture.c`), `SCAN_CHUNK` blocks at a time, and with `dst` also its copy into staging, which the render
thread did alone before. Once a worker finds a block that is not opaque, the chunks left are copied without the check,
or not read at all without `dst`.

- `MT_MIN_BLOCKS` (1024): below this, waking the pool costs about as much as the decode. `MT_MAX_THREADS` is 8.
  `SCAN_CHUNK` (16384) is the blocks a worker checks at a time (256 KiB); `MT_MIN_SCAN` is 4 of them.
- The pool `P`: `busy` is held by the caller for the whole image; `threads` are the workers, the caller not counted;
  `gen` is bumped per image, and a worker joins each generation once; `active`, the workers that have not finished the
  current generation; the current image is written under `lock` before `gen` is bumped; `next`, the first block row (in
  a scan: chunk) not yet claimed; `scan`, the current job: 0 a decode, 1 an opaque check; `sn`, `sdst`, `smax` and the
  result: a scan's blocks, where they go, the most decodes.
- `decode_rows()`: block rows [j0, j1) of the current image, as `astc_decode_ldr`'s loop does them.
- `claim_scan()`: chunks of the current scan, until they run out; without `dst`, also once a block that is not opaque
  was found.
- `pool_init()`: hardware threads minus 2 (the render and the main guest thread), 1 to `MT_MAX_THREADS`; the caller is
  one of them.
- In `astc_decode_ldr_mt()` the rows are cut into (threads + 1) * 4 parts: a few chunks per thread evens out uneven
  blocks.

## `runtime/gfx/tex_deswizzle.c`

Tegra X1 block-linear to linear copies and back. The licence notice in its second and third lines stays in the code.

The layout (from public descriptions):

- a GOB is 64 bytes x 8 rows = 512 bytes; the offset of (x byte, y row) within a GOB is
  `(x%64/32)*256 + (y%8/2)*64 + (x%32/16)*32 + (y%2)*16 + x%16`;
- a block is 1 GOB wide x 2^th GOBs high x 2^td GOBs deep, ordered y first then z inside a block; blocks fill the level
  in x, y, z row-major order.

Level and layer start offsets and the block height and depth of a level come from `runtime/nvn/nvn_tex.c`; this file
only does the addressing inside a level and the copy.

Switch 2 (NVN2 titles, NVN API major version 200 and up) stores the 16-byte pieces of a GOB in another order:
`(x%64/32)*256 + (y%8/4)*128 + (x%32/16)*64 + (y%4)*16 + x%16` (four rows of one 16-byte column before the next
column). Found on Ocarina's packaged textures: with it the ASTC 4x4 title logo and BC4 masks decode without seams (an
edge-continuity search over all 120 orders of the five address bits picks it for both; round 18 of the bring-up notes,
which are not in this repository). Block and level arithmetic are unchanged.

### `nvn_bl_offset()`

The terms of the sum: 512 * bh * bd is the bytes in one block; then which block layer (along depth), which block row,
which block column, which GOB within the block, and the offset inside the GOB.

### `nvn_deswizzle_geom()`

The geometry of one level: bytes per row of blocks, block rows, slices, where the level and layer start in the storage,
and the level's block height and depth. -1 for textures the copy does not handle: targets 5, 6 and 10 (multisampled and
BUFFER), LINEAR storage and more than one sample.

### `bl_copy_rows()`

`nvn_bl_offset` per row instead of per 16 bytes: within a row, GOB column k starts k blocks further, and the four
16-byte segments of a GOB row (x % 64 = 0, 16, 32, 48) sit at 0, 32, 256 and 288 of it (Switch 2: 0, 64, 256, 320).
Rows [r0, r1) of the slices come one after the other (row z * rows + y): each row finds its own place in the
block-linear storage, so any range of rows copies on its own, and `tex_bl_copy_mt()` hands ranges to several threads. A
row may end in a partial GOB.

### `tex_bl_copy_mt()`

`tex_bl_copy()` over the `par_jobs` pool: a level of `BL_MT_MIN` (1 MiB) bytes or more (smaller ones gained nothing:
0.5 MiB took 0.01 ms either way, 2 MiB went 3.3 times faster, 16 MiB 2.5 times) goes as bands of rows, about
`BL_MT_BAND` (128 KiB) bytes each, so a large texture's copy out of the game's memory (the upload's storage copy, 75%
of its CPU time in an area's streaming) does not run on the drawing thread alone. The bytes written are those of
`tex_bl_copy()`.

### `nvn_texels_box()`

A run of bytes is contiguous up to the end of its 16-byte segment, so the box is copied in such runs.

## `runtime/gfx/tex_probe.c`

`nx2saria.exe --probe-vkmem`: texture memory checks. The CPU part: block-linear copies against an independent
reference, ASTC void-extent blocks, and the NVN to VkFormat table against GPU0. The GPU part: the guest heap imported
as a VkBuffer, the write watch against CPU, GPU and ReadFile writes, the virtual-pool page table, and textures uploaded
from block-linear storage and read back, among them ASTC textures at the texture detail in effect
(`NX2SARIA_TEXTURE_DETAIL=reduced` or `low` to check the levels left out). The reference swizzler walks the GOBs in
storage order and places each byte by bit operations, so it shares no addressing code with `nvn_bl_offset`.

### The block-linear checks

- `gob_bits()`: the byte offset inside a 64 x 8 GOB from the bit layout: x[3:0] to bits 0-3, y[0] to 4, x[4] to 5,
  y[2:1] to 6-7, x[5] to 8. Bit by bit, independent of `tex_deswizzle.c`'s arithmetic: Tegra X1, or Switch 2 for NVN2
  titles (the order `tex_deswizzle.c` describes).
- `ref_swizzle()`: `lin` is slices * rows * row_bytes (tight); `bl` the block-linear storage, pre-filled with a marker.
  The GOBs of a block go y fastest, then z.
- `test_deswizzle()`: a level 61 rows high that spans several blocks in x and y; one block; 3D with depth 5 against
  block depths 2, 4 and 8; levels of 256 KiB and more (1000 * 4 by 300, and 97 * 16 by 70 by 9). Then through a
  descriptor: RGBA8 (format 0x25) 2D 200x150 with 4 mip levels, level 1 and 2, layer 0, against the reference on the
  same geometry. Bytes where both fills agree were written by the reference; the untouched storage must hold the same.
  Swizzling the linear copy back leaves the storage unchanged.
- `check_box()`: `nvn_texels_box` against the whole-level path on random boxes: a box read equals the box cut from the
  deswizzled level, and a box write leaves the storage as deswizzle, overwrite the box, swizzle does (nothing outside
  the box changes).
- `test_box()`: one format per block size: 1, 8 and 16 bytes per texel, 8 and 16 bytes per 4x4 block.

### `void_block()` `test_astc()`

`void_block()` starts from 0xFFFFFFFFFFFFFDFC: the 0x1FC mode, LDR, reserved bits 11, an extent of all ones; 0x200
makes it HDR.

- A 3x2 grid of void-extent blocks cropped to a size that cuts the last column and row. UNORM16 = c * 257 gives back c
  in the top 8 bits, plus noise in the low byte that must be dropped.
- An HDR void extent is an error block in LDR: magenta, and `astc_decode_block` reports it.
- An sRGB void extent: the colour is the stored top byte as well (no transfer curve for constant blocks).
- The worker-pool decode gives the same bytes: random blocks (every mode, error blocks included), cropped edges, at
  1021 x 517 for each of the 14 footprints.

### `test_formats()`

The GPU-side promises of the fallbacks: RGBA8 and D32S8 must be usable, otherwise the fallback is empty.

### `test_import()`

A CPU-visible pool over the guest heap: GPU writes land in guest memory, CPU writes are read by the GPU, the write
watch reports the CPU writes and the explicit ones. GPU writes are noted and reported as the replay does
(`vk_mem_gpu_pending`, `vk_gpu_written` at the fence), and a GPU read follows `vk_mem_shadow_sync` as a submit does:
with shadow storage that is what moves the bytes; with an import they are already in place.

- What the CPU wrote before the pool existed is still reported once its pages are imported (or copied into a shadow):
  making the pool must not take the write watch's record of those writes, which the first sync of a mirror and the
  texture checks read afterwards. Every page is touched first, so that the count printed tells what making the pool
  marks by itself.
- As a submit does: a new write-watch epoch (the copy-in reads each region's snapshot once per submission, so a second
  sync in the same epoch would not see the CPU writes made since the first), then the part's CPU inputs go in before
  its GPU work.
- CPU writes (0x4000 bytes at 0x30000) are reported exactly, and the GPU reads them through the same buffer.
- A GPU write noted for the part being submitted must not keep the part's CPU inputs out: a buffer the shaders add to
  and the CPU resets (the game's lighting accumulators) depends on the reset reaching the GPU first.
- Generations: written after `since` gives 1, then 0; an explicit host write gives 1.

### `test_snap_runs()`

The write-watch snapshot of many ranges at once (`vk_mem_snap_note_locked` and `vk_mem_snap_noted_locked` in
`runtime/vk/vk_memory.c`), as the texture pool's freshness check asks for it: ranges in three adjacent 2 MiB regions of
the heap and in one apart (the offsets 0xe10000, 0xa80000, 0x603000 and 0x9f0000: regions at 14, 10, 6 and 8 MiB),
noted in no address order, take one query per run of adjacent regions; afterwards every region has the epoch's
snapshot, the regions with a CPU write since `since` say so, the one without does not, and the pages are exact. The
bytes written are the ones already there, so the tests after this one find their memory as it was. The first query
clears what was written before this test; a region noted twice is asked once.

### `test_remade()`

A GPU write is noted on a pool, and the pool is finalized and made again at its address before the write is reported:
the report must leave alone what the CPU has written to the new pool since. With shadow storage the new pool's shadow
is a copy of guest memory from before those writes and holds no byte of the old write (`shadow_write_back` in
`vk_memory.c`); with an import the report copies nothing. The steps: the submission being recorded writes the pool's
first two pages; the pool is made again; the CPU writes the first page of the new pool; the old submission's fence
reports the write.

### `test_table_mirror()`

A pool in the device address table gets its VRAM mirror while a guest submission is recorded, as on a draw's first use
of it as a buffer (`mirror_create` in `vk_memory.c`). The table takes the mirror up by a store the GPU runs after the
mirror's first copy (`vk_gpa_flip_cmd` in `runtime/vk/vk_gpa.c`), so nothing is changed when the mirror is made; read
from the table's own memory once the submission is done, the pool's page reads the mirror and writes guest memory as
well, and the mirror holds the pool's bytes. A read while the submission may still be in flight gives the table as the
CPU left it when the mirror was made; without the late store (`vk_mem_gpa_late`) the table changes when the mirror is
made, as up to 0.9.2.

### `table_part_record()` `table_part_of()`

For `test_table_sync()` and `test_table_store()`: a guest submission of one part that records nothing. `bound`: its
record binds the device address table, as the replay of a draw whose shader reads through the table does (`ssbo_info`
in `runtime/vk/vk_exec_draw.c`), so its prologue brings every shadow of the table up to date (`vk_mem_shadow_sync`).
Its completion callbacks have run when the fence wait returns.

### `test_table_sync()`

Shadow storage: a pool that no part ever binds or resolves, as the pools a shader reaches only through a pointer it
read from another buffer. Its shadow, which the device address table names, starts with the bytes guest memory held
when the pool was made; a CPU write reaches it at the next guest submission whose record binds the table, and not at
one that does not (nothing else brings it there, which is what left the foliage's instances stale). The shadow is read
back with a GPU copy out of the buffer a lookup names, which does not touch the pool.

The values: 0x13579BDF before `vk_pool_init` (a shadow starts with the bytes an import would show); 0x2468ACE0, a plain
CPU write: nothing resolves, uses or flushes the pool; 0x0F1E2D3C, again, a guest submission later.

### `test_table_store()`

Shadow storage: what a shader stores through the device address table lands in the pool's shadow copy and nothing
notes it ("Pages a shader stores into" in `vk_memory.c`). A GPU copy into the shadow buffer that is not noted stands in
for the store. Word X of a page takes the stores, the CPU rewrites word Y of the same page before every part:

- the store made after the page's first whole copy-in is overwritten by the second, which finds it (nothing can keep
  it);
- every later store stays in the shadow through the copy-ins that bring Y, and is in guest memory after the fence;
- a CPU write of X after that reaches the shadow.

The parts in order: the page's first whole copy-in (its mark); a store of 0xAAAA0001 and a part that finds the store,
overwrites it, and leaves the page a base copy; a part that brings Y and keeps X, whose fence writes X back; the CPU
resets the stored word; and a store after the reset (0xAAAA0003) comes back as well.

### `test_readfile()` `test_virtual()`

`test_readfile()`: ReadFile into a watched page, buffered and unbuffered: timed, and whether the write watch saw the
kernel's write. `test_virtual()`: a physical pool, a virtual pool mapping 64 KiB pages of it, resolution and unmapping;
the GPU writes through a virtual address and the bytes are read through the physical one.

### `make_tex()` `fill_storage()` `image_is_color()` `image_bytes()`

- `make_tex()` sets `pool_flags` as nvnTextureInitialize records them: `tex_mirrored` (`vk_texture.c`) reads them, not
  the pool object.
- `fill_storage()` fills every level of `t`'s storage with random texels through the reference swizzler; `lin[lv]`
  keeps them tight.
- `image_is_color()`: level 0 of `image` (w x h RGBA8) read through a staging buffer after everything recorded so far
  has run: whether every texel is `want`, the way a draw sampling the image would see it.
- `image_bytes()`: level 0 of `image` (w x h RGBA8) read into `out` the same way.

### Texture detail

An ASTC texture made without its top mip levels (the texture detail of `runtime/vk/vk_texture.c`).

- `detail_color()` and `detail_fill()`: every level and layer of `t`'s storage becomes ASTC void-extent blocks of one
  colour, each channel 0 or 255 so that the RGBA8 decode and its BC3 encoding both hold it exactly. The colour says
  which level and layer it is; it is never black (k from 1 to 7).
- `detail_readable()`: the image formats of an ASTC texture whose levels `detail_level_is()` can read: the RGBA8
  decode, its BC3 encoding, and the BC1 one an opaque texture gets (`astc_reformat` in `vk_texture.c`; these cases'
  blocks are opaque). An image in the ASTC format itself, where the device samples it, is not read here.
- `detail_level_is()`: whether level `lv`, layer `ly` of the image (RGBA8, BC3 or BC1, w x h at that level) is `want`
  in every texel. BC3: two 5:6:5 endpoints at bytes 8 to 11; both are the colour, so every texel is. BC1: the endpoints
  at bytes 0 to 3, then 2 bits a texel; with equal endpoints the selectors 0 to 2 all give the colour, and 3 is black
  in this format, so none may be 3.
- `detail_case()`: the image of an ASTC texture (format 0x80, ASTC 8x8) has the game's levels from the skip the setting
  gives it: its size is that level's, and each of its levels and layers holds that level's blocks, after the upload and
  after a rewrite of the storage. A native ASTC image is not checked.
- `detail_retry()`: an ASTC upload whose GPU decode gets no scratch memory (`scratch_get` in `runtime/vk/vk_astc.c`, as
  when the video memory is used up; forced here through `vk_astc_scratch_fail`) does not count as done: the texture
  stays "not uploaded", is left alone for the frames of the wait, and the first lookup after them uploads it, after
  which the image holds the storage's blocks. Skipped (0) where ASTC is not decoded on the GPU: there is no scratch
  buffer then. The case is a 128x128 ASTC 8x8 texture; a lookup inside the wait leaves it alone; the frame counter is
  moved on by 30 (`UP_RETRY_FRAMES` of `vk_texture.c`); once uploaded, a further lookup does nothing more.
- `test_detail()`: 1024x1024 with 6 levels (two levels less at low), a 256x256 array of 3 layers (one less below full),
  and the ones the rule leaves whole: 128x128, and 512x512 without smaller levels. Then the sampler of a texture made
  without levels: an open LOD clamp is the slot's sampler, a closed one moves down.

### `test_textures()`

- Block-linear RGBA8 (0x25), 200x120 with 3 levels, in a CPU-visible pool: uploaded through the GPU, read back from the
  image.
- CPU writes to the storage: the write watch, then a re-upload on the next use, and every mip the GPU sees has the new
  bytes. The exact count of re-uploads is checked on Windows only: elsewhere an import's pinning can mark pages written
  again without a CPU write.
- The same bytes written again, so that every page of the storage shows a write. The texture is over 64 KiB: with
  `NX2SARIA_TEX_SAME_BIG` (the default) it is compared by content and left as it is, with 0 it is uploaded again. Other
  bytes upload either way, and the image holds them. After two comparisons in a row that found other bytes its uploads
  take no hash any more, so the same bytes written again then upload with the switch too.
- BC1 (0x42), 64x48: blocks uploaded as they are, read back from the image.
- RGBA8 64x64 in a physical pool: it aliases the pool memory; WriteTexels then ReadTexels through the image.
- A finalized CPU-backed slot must not revive its stale bytes or follow a new, unrendered CPU texture.
- Registration is the lifetime boundary that makes the descriptor current again, even after its image was evicted.
- A release finalizes only the slots of its own texture object: another texture initialized at the same storage with
  the same parameters before the finalize keeps its slot (`vk_texture_release`, terrain heightmaps).
- Its own slot 5 is finalized. The texture's image still holds the texture (slot 6's texture keeps it current), so with
  the kept-stale rule the slot shows that image (`test_stale_kept()`, `stale_kept` in `vk_texture.c`); without the rule
  it resolves to nothing.
- Once the live texture at its storage holds GPU content the storage bytes lack, the finalized CPU slot follows it.
  The live texture (16x12) is cleared on the GPU, to (0, 1, 1, 1), and noted as rendered into, as a draw into it would
  be (`vk_texture_gpu_written`: the clear is its content, not the storage); the slot's image must then show the clear
  (00 FF FF FF), which neither the storage bytes nor a recreated image of the slot's own descriptor hold.
- GPU-owned slots do follow the live image at their storage, even when its descriptor has changed, but only one of
  their own size (`follow_size_ok` in `vk_texture.c`): the game's render targets share storage, and the live one at an
  address is often a target of another size that a frame laid out there. The 64x64 slot gets the image of a 64x64
  texture of another format; once that one is released and a 32x32 one is live there, the slot gets an image of its own
  descriptor. (In the other order the slot's own entry is live again after the second step, and a slot whose own entry
  is live keeps it.)

### `test_stale_kept()`

The kept-stale rule (`stale_kept` and `slot_resolve` in `vk_texture.c`; `NX2SARIA_TEX_STALE_KEPT`, default on). The
slot of a texture the CPU writes, finalized with its texture, goes on showing that texture's own image while the image
still holds the texture: the game finalizes ahead of the replay of the frames it recorded before. The image is sampled
as it is, so bytes the storage gets after the finalize do not reach it and nothing is uploaded. With the switch off the
slot resolves to nothing, which a draw shows as the stand-in. Either way a registration makes the descriptor current
again, with the storage's bytes as they are then. That another descriptor made at the storage ends the rule is the
"stale CPU slot" case of `test_textures()`.

The second fill is what the game puts into the storage next. The registration is checked in a later submission: an
entry's storage is checked for writes once per submission (`vk_texture_by_id_locked`), and this one's was at the first
lookup.

### `split_case()` `test_split()`

Two descriptors on one CPU-written storage (the split fill of `vk_texture.c`, `start_at`). D1 is a texture the CPU
writes. D2 names the same storage with a texture flag for which the cache does not take the content from the CPU
(COMPRESSIBLE here), so a slot of D2 shows D1's image for as long as D1 has a live entry. What the slot shows once D1's
entry is gone, in the two orders a game can let go of them:

- keep: D2's slot stays registered while D1 is released and registered again (a texture made again in place);
- both: D1 and D2 are released, the slot of D2 is looked up all the same (the texture pool refresh looks at every
  registered slot), then the storage gets new bytes and both are registered again (the next texture in that memory).

`recorded`: the lookup that makes D2's entry runs while a submission is recorded, as a draw of a replay makes it, so a
copy from D1's image can be recorded there (`split_record()`, the frame of a recorded submission, as
`gpa_read_record`); otherwise outside any recording, where only the storage can fill it. `*what`: 0 the slot's image is
all zero, 1 it holds the storage's bytes as they are at the end, 2 the bytes the storage held when D2's entry was made
(the same as 1 in "keep", where the storage is not rewritten), 3 none of these. The textures are 32x32 (`SPLIT_W`,
`SPLIT_H`). At the end a shader "sampled" the slot: with `NX2SARIA_TRACE=perf` the log names an entry that never had
content.

`test_split()` runs the three cases (keep, both, keep with the lookup recorded) with split fill off (`tex_split_fill =
0`), on (1) and filling once (2). Off, the slot shows an image nothing filled, which is what the setting is for (zeros
with `NX2SARIA_FIX_ZERO`, the default; else whatever the memory held); on, the storage's bytes, copied from D1's image
where a submission was being recorded and uploaded from the storage elsewhere; filling once, the bytes of the texture
that was in the memory before when both descriptors were let go of. `vk_texture_split_fill_set(-1)` lets the setting
decide again. The copy from D1's image in a recorded submission is checked on Windows only: elsewhere an import's
pinning can mark pages written without a CPU write, and the storage is uploaded.

## `runtime/gfx/astc_probe.c`

`--probe-astc`: the compute ASTC decoder (`runtime/vk/vk_astc.c`, `runtime/shaders/astc_decode.comp`) against
`astc_decode_ldr`, bit for bit, for every block footprint, linear and sRGB. Synthetic blocks only (no game files):
valid void extents, invalid and HDR void extents, random blocks kept by what they exercise (1-4 partitions, dual plane
with 1-3 partitions, every weight grid and quantization the random modes reach), then plain random blocks (mostly
error blocks, HDR endpoint modes among them). Each case decodes two regions into two mip levels of an RGBA8 image with
cropped edges, and compares both the decoder's texels (readback) and what the image holds afterwards. The same regions
also go through the BC3 encoder (`NX2SARIA_ASTC_BC`) into a BC3 image, whose blocks are decoded here and held to error
bounds (`bc_check()`). Then cut-out foliage at every footprint (`cutout_case()`): full mip chains of transparent and
opaque patches with sharp alpha edges, through the encoder, where alpha 0 and 255 must stay exact and alpha tests keep
their outline (`cut_check()`). Every block of both is also put to `astc_block_opaque` of `tex_astc.c`, which must say
what a decode says (`opaque_check()`), and with `NX2SARIA_ASTC_BC1` the regions are encoded to BC1 as well: each BC1
block must be the colour half of the BC3 block made from the same texels (`bc1_check()`), the four-colour block BC1
decodes alike. It also times one 2048x2048 texture on the CPU (`astc_decode_ldr_mt`) and on the GPU (wall time of the
upload batch).

### The blocks

- The buckets of valid blocks (`NB` 7, `PER_BUCKET` 48 each): partitions 1-4 single plane, then dual plane with 1-3
  partitions. `g_bc_db` is the lowest BC3 PSNR of all cases.
- `dual_bit()`: the 6..10 x 6..10 modes have no dual plane.
- `void_block()`: kind 0 a valid LDR extent (random coordinates s0 < s1, t0 < t1), 1 an all-ones extent, 2 s0 >= s1
  (an error), 3 HDR (an error), 4 reserved bits not 11 (an error).
- `make_blocks()`: `n` blocks for footprint `bw` x `bh`: the buckets first, void extents, then random.

### `gpu_decode_ar()` `gpu_decode()` `test_bl()`

- `gpu_decode_ar()` decodes the two regions `ar` (w0 x h0 at level 0, w1 x h1 at level 1) of the `src_bytes` at `src`
  on the GPU; `out_rb`: the decoder's texels, `out_img`: the image's two levels read back. 0 on a Vulkan failure.
- `gpu_decode()`: the two regions as tight blocks: `n0` blocks of level 0, then `n1` of level 1.
- `test_bl()`: the same blocks tiled as the game stores them (`tex_bl_copy`, the CPU deswizzle's own copy), level 0 in
  blocks of 4 GOBs and level 1 in blocks of 2, decoded in place (`VkAstcRegion.bl`, `bl_block` in `astc_decode.comp`):
  the decoder's texels must be `want`'s. It checks the GPU's block-linear addressing against the CPU's.

### `gpu_bc()` `bc3_decode()` `bc_check()`

`MAX_LEVELS` (13) is a mip chain of up to 4096 texels a side.

- `gpu_bc()`: mip levels 0 .. levels-1 (`w[i]` x `h[i]` texels from the `n[i]` blocks that follow the previous level's
  in `blk`) decoded and encoded to BC3 (`bc` 1, `NX2SARIA_ASTC_BC`) or BC1 (`bc` 2) by one `vk_astc_record` call into an
  image of that format; `out`: its blocks read back level after level, rows of 4x4 blocks. 0 on a Vulkan failure or
  when nothing was recorded.
- `bc3_decode()`: one BC3 block to 16 RGBA8 texels, as the format defines it (565 endpoints widened by bit replication;
  the colour block always in four-colour mode). `pos`: each texel's place on the line from endpoint 0 (0) to endpoint 1
  (3).
- `bc_check()`: BC3 is lossy, so these are bounds, not bit-exactness. In every block: alpha within a fourteenth of the
  block's alpha range (the eight-value mode's half step); each texel on the palette entry nearest its projection onto
  the endpoint line (within rounding, when the endpoints are apart); a block of one colour within the 565 rounding,
  alpha exact. Per level, a PSNR floor against `astc_decode_ldr` for gross damage (`BC_MIN_DB`, 15): the random blocks
  are noise, so it is low (19 dB at worst for 5x5 level 1 on an RX 9070). `*db`: the level's PSNR. The projection is
  checked for endpoint lines of squared length 64 or more: on shorter lines rounding alone can move a texel to the next
  entry. A texel more than 0.75 from its projection fails: a wrong code mapping is a whole place off.

### `opaque_check()` `opaque_mt_case()` `bc1_check()` `bc1_case()`

- `opaque_check()`: `astc_block_opaque` against a decode of each of the `n` blocks: opaque exactly when every texel has
  alpha 255. Then the loop over them (`astc_blocks_opaque`, also copying) and the worker-pool scan
  (`astc_blocks_opaque_mt`) must give the first block that is not opaque, the same bytes, and the same answer. Opaque
  4x4 blocks are kept for `opaque_mt_case()` (`OP_POOL`, 4096).
- `opaque_mt_case()`: the worker-pool scan of a texture of 300,000 opaque 4x4 blocks (opaque blocks of the cases,
  repeated), alone and copying: opaque, every byte copied; then with one block that is not opaque at 250,000: not
  opaque, every byte still copied, and the loop names that block.
- `bc1_check()`: the BC1 blocks (8 bytes each) of a level against its BC3 blocks: each the second half of its BC3
  block. `bc1_case()`: the regions of one case encoded to BC1 too (`vk_astc_bc1`), held to `bc1_check()` against their
  BC3 blocks.

### `test_case()`

Level 0 is 37 x 23 blocks, cropped by 1-3 texels; level 1 half of that, with other blocks.

### Cut-outs

Alpha-tested foliage through the BC3 encoder: mip chains whose ASTC blocks are, by a hash of their place, fully
transparent (alpha 0 over whatever colour, as an encoder leaves the colour under a cut-out), fully opaque leaf green,
or a block whose alpha jumps from transparent to opaque between two neighbouring texels. The edges between blocks lie
on the footprint's grid, so along a side of 5, 6 or 10 texels they cut through BC3's 4x4 blocks at every offset; the
levels shrink to odd sizes down to 1x1. `g_cut_db` is the lowest BC3 PSNR of the cut-out levels.

- `sharp_blocks()`: valid blocks (not void extents) whose alpha runs from at most 16 to at least 240 with a step of 192
  or more between two neighbouring texels; the number found, up to `SHARP_POOL` (96).
- `void_rgba()`: a void-extent block of one colour (UNORM16 channels, the 8-bit value in both bytes).
- `cut_blocks()`: the `n` blocks of one level, `bx` to a row: opaque and transparent in patches of 2x2 blocks, one
  block in five from the sharp-edged pool. A transparent block takes any colour.
- `cut_check()`: what an alpha test sees of the cut-outs. A texel the ASTC image holds fully transparent or fully
  opaque stays exactly 0 or 255 (a BC3 alpha block's endpoints are its 4x4 block's least and greatest alpha), and every
  texel keeps its side of an alpha test at a quarter, a half and three quarters (64, 128, 192) unless it lies within
  the block's alpha step of the threshold (the margin is `bc_check()`'s alpha bound, the range / 14 + 1). `*edges`: the
  4x4 blocks whose alpha runs from at most 16 to at least 240.
- `cutout_case()`: one cut-out mip chain of w0 x h0 texels at footprint `bw` x `bh`: every level held to `bc_check()`'s
  bounds and `cut_check()`.

### `timing()` `probe_astc()`

`timing()`: one 2048 x 2048 texture of valid blocks: the CPU worker-pool decode against the GPU time of the dispatches
alone (the timestamps of `vk_astc.c`, `NX2SARIA_ASTC_TIME`) and the wall time of the upload batch. It makes 4096 valid,
non-constant blocks: error blocks would flatter the GPU.

`probe_astc()` sets `NX2SARIA_ASTC_BC` to 1, which also builds the BC3 encoder; the RGBA8 cases pass `bc` 0 and do not
use it. The cut-outs run at every footprint (14), then as two chains large enough that level 0 is a scratch chunk of
its own.
