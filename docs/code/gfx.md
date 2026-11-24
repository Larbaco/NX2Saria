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
