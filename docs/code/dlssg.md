# runtime/dlssg

`nx2navi_dlssg.dll`: DLSS Frame Generation through NVIDIA Streamline, behind a small C interface that the game's
Vulkan back end loads (`runtime/vk/vk_dlssg.c`). What frame generation is and how the game reports it:
[frame-generation.md](../frame-generation.md), [architecture.md](../architecture.md), the `dlssg:` lines in
[logging.md](../logging.md).

The DLL is built with MSVC against the Streamline SDK's headers (`tools/build_dlssg.py`; the SDK stays outside the
repository), so its C++ types have the ABI of Streamline's own DLLs. `sl.interposer.dll` and its plugins are loaded from
the game's folder at run time; this DLL links none of them. The game itself is built without the SDK: it only sees
`runtime/dlssg/nx2_sl.h`. On Linux the game does not use this DLL (DLSS-G runs through NGX there,
[README-BuildKit.md](../README-BuildKit.md)).

## `runtime/dlssg/nx2_sl.h`

The C interface of the DLL, which `runtime/vk/vk_dlssg.c` loads. `NX2SL_VERSION` is the interface's version. Each
export has a `PFN_nx2sl_*` pointer type; what each one does is under its function in `runtime/dlssg/nx2_sl.cpp` below.
The exports marked optional there are missing from an older DLL, and the game then goes on without them.

- `Nx2SlImage.layout`: the layout the image is in when Streamline reads it (at the present).
- `Nx2SlCamera`: the four matrices are row-major; `jitter` is in render pixels; `mv_scale` is what makes the motion
  vectors fractions of the render size.

## `runtime/dlssg/nx2_sl.cpp`

Manual hooking (the SDK's ProgrammingGuideManualHooking): the game creates its instance and device through the
interposer's `vkCreateInstance` and `vkCreateDevice`, which add what DLSS-G needs, and calls the interposer's swapchain
functions; everything else goes to the Vulkan loader as before. Each frame of the game sleeps in Reflex where it starts
and gets the PCL latency markers, the camera constants and the depth and motion vector tags (valid until the present),
then the present goes through the interposer, which inserts the generated frames: one, two or three between two
rendered frames (2x, 3x or 4x), up to what the GPU and driver allow (`numFramesToGenerateMax`).

State: `g_low_latency` is whether Reflex's low-latency sleep runs on this GPU and driver (`ReflexState`), -1 while not
asked; `g_frames` is the number of frames to generate as last set (0 off), -1 before the first `nx2sl_frame`; `g_max` is
`numFramesToGenerateMax` as Streamline last reported it, 0 before it did.

### `sl_log()`

Streamline's warnings and errors from its start until the verdict on DLSS-G (`nx2sl_available`,
`nx2sl_log_unavailable`) are held (`g_held`, at most 32; one past that room is written at once). On a GPU without DLSS-G
Streamline writes the same seven at each start (DLSS-G disabled on this hardware, its plugin ignored, hooks not
supported, its context missing), and the support check's "no supported adapter" explains them: one line sums them up
then (`release_held`). Any other verdict writes them as they came, in order, and so does the shutdown when no verdict
came. Only warnings and errors reach the game's log; info messages are dropped. At most 200 lines are written
directly.

### `release_held()`

Writes the held messages and stops holding. `no_adapter`: the support check found no adapter with DLSS-G, and the
messages that come from that (the `known` table) are counted in one line; any other is still written as it came.

### `reflex_state()`

Whether Reflex's sleep runs on this GPU and driver: Streamline takes a frame limit without it and then sets nothing (its
`slReflexSetOptions` hands the limit to the driver only with the low-latency sleep available). Asked until Streamline
answers, from `nx2sl_available` and `nx2sl_reflex2` (and `nx2sl_reflex`, which an older runtime calls). The runtime calls
those on the device's thread before any present or under its present lock (`runtime/vk/vk_dlssg.c` `reflex_apply()`),
never two at once: `slReflexGetState` is not thread safe.

### `token()`

The token of frame n. Tokens come from a small ring Streamline reuses, so each call takes its own right before use.

### `nx2sl_init()`

`slInit` with DLSS-G, Reflex and PCL for Vulkan; `dir` is the folder of `sl.interposer.dll` and the plugins (the
game's). The preference flags: the game tracks its own command buffer state (`eDisableCLStateTracking`), manual
hooking, and no plugin downloads at run time; the depth and motion vectors are tagged per frame (`slSetTagForFrame`,
`nx2sl_frame`), which Streamline refuses without `eUseFrameBasedResourceTagging`. `applicationId` 231313132 is the id
NVIDIA's samples use for builds without an issued one (as the NGX of `runtime/vk/vk_dlaa.c` does).

### `nx2sl_instance_proc()`, `nx2sl_device_proc()`

The interposer's proc address functions: `vkCreateInstance` and `vkCreateDevice` through them add what DLSS-G needs
(extensions, features, queues) and tell Streamline the device; the swapchain functions through them are its hooks.

### `nx2sl_available()`

1 when DLSS-G loaded and supports this GPU, asked after the device exists. How many frames this GPU and driver generate
per rendered frame is known once the device exists, else after the first presents (`nx2sl_present_marker`). When DLSS-G
is unavailable the held Streamline messages wait for `nx2sl_log_unavailable`, which has the verdict.

### `nx2sl_log_unavailable()`

Optional. Logs Streamline's support result for frame generation after availability failed: its general check of the
system, driver and adapters. Streamline looks at one adapter only when given its LUID, which Vulkan has not. The verdict
is kept for `nx2sl_support` (`g_support`, `g_support_name`).

### `nx2sl_support()`

Optional. What that check said: -1 not asked (or no query), 0 no adapter here has DLSS-G (`eErrorNoSupportedAdapterFound`
or `eErrorAdapterNotSupported`: an RTX 30 or older, or another vendor's card), 1 supported, 2 another reason, whose name
(`sl::getResultAsStr`) goes into `*name`.

### `nx2sl_reflex2()`

Reflex's low-latency mode (0 off, 1 on, 2 on with boost) with Reflex's frame limit, `frame_limit_us` microseconds from
one frame's start to the next (0 none), which the driver's sleep then holds (`runtime/vk/vk_dlssg.c`
`vk_dlssg_pace()`). Called after the device and after each swapchain the game creates through the interposer: the mode
is the swapchain's. Streamline sets the mode and limit on the swapchain it was told of last (`VK_NV_low_latency2`, the
sleep mode's minimum interval) and drops them while there is none, so both are set again after each swapchain. Every
call carries both, so the runtime calls only this one when the DLL has it: a call without the limit clears it.

Returns 1 when Streamline took them, unless Streamline said Reflex's sleep does not run here (then nothing would hold
the limit). Where it has not said, the runtime's own check of the frames (`runtime/pacing/dlssg_pace.h`) finds a limit
nothing holds. Optional: an older DLL lacks it, and the game's own limiter then keeps the frame limit. The log line is
written for the first 8 calls and for later ones Streamline refused (up to 40).

### `nx2sl_reflex()`

The export of the runtime before `nx2sl_reflex2`: the mode alone, no frame limit.

### `nx2sl_sleep()`

Frames are numbered by the game's presents: frame n ends with its n-th present (`runtime/vk/vk_dlssg.c`
`vk_dlssg_game_present()`). This is where the game starts a frame: with `hold`, Reflex's sleep, which holds the start
until the time the driver computes from the measured render and queue times; then the frame's simulation and
render-submit-start markers. The game's simulation and its recording of the frame's commands are one span here: the
guest's threads cannot be told apart, so the simulation markers sit at the frame's start and the render submission runs
until the frame's submissions went out (`nx2sl_submit_end`).

### `nx2sl_submit_end()`

The frame's submissions went out (at the game's present; with asynchronous replay, when the replay reaches it): its
render-submit-end marker.

### `nx2sl_frame()`

Each host present, before the present call: the frame's constants and tags, and frame generation on or off. `enable` is
the number of frames to generate between two rendered ones (0 off; 1, 2 or 3 for 2x, 3x or 4x), at most what
`nx2sl_frames_max` reports; a DLL without `nx2sl_frames_max` (an older build) generates one for any nonzero value. Then
`nx2sl_present_marker(0)` before the present and `(1)` after it. Never more than Streamline allows: one while it has not
said.

A frame without depth, motion vectors or camera while generation is on (a menu, a fade or a load, while
`runtime/vk/vk_dlssg.c` holds the change of state for a few frames): Streamline's guide asks for the common constants at
every frame and for null tags wherever depth or motion vectors are not valid (ProgrammingGuideDLSS_G.md 5.2 and 7.0).
Such a frame gave neither, and Streamline logged "Unable to find 'common' constants for frame" and "Failed to find global
tag 'kBufferTypeDepth'" and went on with what the frame before had set. It now gets the last frame's constants again
(`last`, the constants of the last frame that had its inputs), as a new start, and the three tags as null.

`eRetainResourcesWhenOff`: generation goes off for every menu, cutscene and frame without depth and motion, and on again
after it. Off frees the generator's resources unless this flag is set, and on allocates them again inside the present,
with the queue lock held: a frame of 100 to 150 ms each time, the submitting thread waiting for that lock. With the flag,
off only suspends. Whether frame generation runs at all is fixed for the run (`runtime/vk/vk_dlssg.c`), so nothing is
held for long that would otherwise be given back; Streamline frees it at shutdown.

### `nx2sl_frames_max()`

Optional. The most frames DLSS-G can generate between two rendered ones on this GPU and driver (Streamline's
`numFramesToGenerateMax`: 1 on a GPU that does 2x, 3 on one that does 4x); 0 until Streamline reports it. Asked after the
device exists and refreshed after each present. An older DLL lacks it, and the game then asks for one.

### `nx2sl_present_marker()`

The present-start and present-end markers. Asked after each present, `numFramesActuallyPresented` is what that present
showed: 2 with one generated frame, 4 with three. A `numFramesToGenerateMax` of 0 now and then is no news; only a new
nonzero value is logged and kept.

### `nx2sl_shown()`

The frames the display got, generated ones included, counted after each present (on the present thread).
