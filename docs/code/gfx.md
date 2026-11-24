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
  `gfx_shader_outputs()`: the Output Locations (vertex varyings, fragment colour targets); the comment that said so
  sat at the end of the line of `gfx_shader_mvprev_banks()`.
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
  frame, RGBA8, shown over the game image while frames keep coming (the game draws movies with its own shaders, which
  do not translate yet). `gfx_movie_frame_own()` is the same but takes the malloc'd buffer, freed later.
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
(`OP_CLEAR_BUF`, which the comment at the top did not list) and uniform updates. It is the CPU-side gate for the NVN
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

NVN2 (Switch 2) programs: a 0xe0-byte control section (magic 0x98761234, the version at +4, the code size including the
0x80-byte shader program header at +0x38, the register count at +0x4c) and SM86 code (128-bit Volta+ instructions after
a Turing-style 0x80-byte header; compute programs have no header). The translator lowers them to Maxwell
(`tools/xlate/AmpereLowering.cs`, job "sm86"); the code is sent from its first byte (the comment at this point also said it has
no c1 part, which the code under it does not do: see the layout below). The control section is the first 0xe0 bytes only (`NVN2_CONTROL_SIZE`): `nvn2_control()` copies them into a zeroed
`CONTROL_SIZE` buffer at the entry points, so the bytes after it (other guest data) stay out of the hashes, stored
inputs and jobs. The layout of an NVN2 program: the code (its size at control +0x38), alignment padding, then the c1
constants (jump tables of BRX, compiler constants) from the offset at +0x40 to the one at +0x44. They are hashed and
sent with the code; when those two offsets do not fit the program, the length is the code size and c1 is empty.

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
