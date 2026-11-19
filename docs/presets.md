# Quality presets

A quality preset is a set of graphics defaults chosen for the PC the game runs on. The setting `preset`
(`NX2SARIA_PRESET`) is `recommended` (the default), `low`, `medium`, `high` or `ultra`. At every start a hardware probe
reads the GPU, the processor, the memory and the handheld the firmware names, and `runtime/preset_pick.h`
`preset_pick` turns them into the recommended tier. **High is the game as shipped and sets nothing.** The other tiers
set the keys of the table `PRESET_ROWS` in the same header.

Code: `runtime/preset_pick.h` (the tiers, the rule and the table; header only, so the tests check it without a device),
`runtime/hw_detect.c` (the probe), `runtime/cfg.c` "quality presets" (the layer, the start's choice, the state file,
the calls the settings menu makes), `runtime/ui/menu.cpp` `row_preset`, `preset_confirm`, `row_scene_res` and
`preset_notice_tick` (the menu), `runtime/ui/option_rows.cpp` (the draft, `opt_preset_handback` and Apply), `runtime/bugcap.c`
`preset_lines` (bug captures). The player's side is in [Controls, "Quality presets"](controls.md#quality-presets).

## A layer of defaults

A setting's value comes from the first of these that has one (`runtime/cfg.c` `cfg_get_now`, `cfg_default`):

1. what the player sets: the environment (`NX2SARIA_*`), then `nx2saria.ini`, which the menu writes;
2. the device profile (`device_profile`, the Steam Deck's defaults: `runtime/cfg.c` `DECK`);
3. the preset in use;
4. the settings table's default (`runtime/cfg.c` `metas`).

Why a layer, and not values written into `nx2saria.ini`:

- **A value set by hand always wins.** The layer sits under it, and the start's log names every key the tier sets
  whose value comes from somewhere else (`preset_log_keys`). The menu's "default" mark follows the layer: resetting a
  key in the menu returns it to the preset's value.
- **Custom.** When the player sets a key the preset in use sets to another value than the one it reads unset (the
  preset's, or the device profile's where the profile sets that key too), the preset counts as custom
  (`cfg_preset_custom`, `cfg_preset_differs`): the menu row shows *… (custom)* with the note *set by hand: …*, and the
  performance overlay's preset line says the same. A hand-set value equal to the preset's does not count, nor does a
  key the tier leaves alone (any key on High). These are the keys a choice of that tier hands back
  ([The settings menu](#the-settings-menu)).
- **The file stays the player's.** Nothing in the detection writes `nx2saria.ini`: the `preset` line is written only
  when the player applies a choice in the settings menu (or edits the file). What the detection itself has to remember
  goes to its own file, `preset-state.txt` ([below](#the-state-file)).
- **Updating changes nothing where the game already ran as shipped.** On a PC recommended High the layer is empty, so
  an update that brings the presets changes no value there. A weaker PC whose player never chose a preset gets Medium
  or Low after the update; a one-time notice and the log say so ([The settings menu](#the-settings-menu),
  [The log](#the-log)).

The layer is one atomic tier number plus the hardware read at start, which never changes afterwards; every value is a
string literal of `PRESET_ROWS`, so readers on any thread take no lock (`preset_layer`).

## What each tier sets

`PRESET_ROWS` has one row per key, tier and kind of hardware; for one key a later row that applies replaces an earlier
one (`preset_value`). "—" is a key the tier leaves alone, so it reads the device profile's or the table's value; the
High column is the table's default, the game as shipped:

| Setting | Low | Medium | High (shipped) | Ultra |
|---|---|---|---|---|
| `scene_share` | 0.5; 0.4 on an integrated GPU, a software renderer or a handheld | 0.67 | 0.75 | 1 |
| `game_res` (the picture's height) | 1080 | 1080 | 1440 | `window` |
| `aniso` | 4 on an integrated GPU, a software renderer or a handheld; else — | — | 16 | — |
| `standin_filter` (what enlarges the scene) | `bilinear` | — | `easu` | — |
| `shadow_pcf` | — | — | 0 | 1 |
| `foliage` | `reduced` on an integrated GPU, a software renderer or a handheld; else — | — | `full` | — |
| `fps_limit` | 30 on an integrated GPU that is no handheld or a software renderer; 40 on a handheld; else — | — | 60 | — |
| DLSS (an NVIDIA RTX card) | `dlaa = 1`, `taa = dlss`, `dlss = performance` | `dlaa = 1`, `taa = dlss`, `dlss = quality` | off | — |
| FSR 4 (a Radeon RX 90x0 card on Windows, no DLSS) | `dlaa = 1`, `taa = fsr3`, `fsr4 = 1`, `dlss = performance` | the same with `dlss = quality` | off | — |

The 3D scene's height at 16:9 that follows: Low 540 lines (432 on an integrated GPU, a software renderer or a handheld), Medium about 720,
High 1080 (the game's 1920x1080 scene in its 2560x1440 picture), Ultra the window's height, at least 1440. With DLSS or
FSR 4 the mode's share replaces `scene_share`: Medium's quality mode renders 2/3 of the 1080-line picture, Low's
performance mode 1/2.

Decisions:

- **High is empty.** The shipped game is the reference every report and measurement so far was made with; a preset
  that equals it cannot make a PC worse than before the presets.
- **No tier turns frame generation on.** It adds latency and needs a base frame rate that the lower tiers exist to
  reach; it stays the player's choice. No row may name `framegen` (the tests refuse one).
- **The frame-rate limit is lowered only on integrated GPUs, software renderers and handhelds**, and never above 60. An
  integrated GPU holds 30 more steadily than an uneven 40 to 60; 40 divides a handheld's 40, 80 and 120 Hz modes. A discrete card keeps 60.
  On the Steam Deck the device profile's 40 is the value in effect (the log says `(device profile)`).
- **DLSS or FSR 4 only below High, and only on the cards that run them** (`preset_gpu_caps`): DLSS on an NVIDIA card
  whose name says RTX; FSR 4 on an AMD Radeon RX 90x0 card (RDNA 4) on Windows, where its library runs. AMD's library
  also lists FSR 4 for the RX 6000 and RX 7000 cards, but there it costs more GPU time than FSR 3.1
  (`nx2saria.ini.example`, `fsr4`), the opposite of what a lower tier is for. Other cards get the scene at
  `scene_share`, enlarged by the stand-in ([below](#rendering-the-3d-scene-below-the-pictures-size)). No tier sets
  FSR 3.1. DLSS comes up only at a start, so a tier that turns it on in a run that started without it gives the plain
  share until the next start, as the Upscaling row does.
- **Ultra only adds**: the scene at the picture's full size, the picture at the window's height, filtered shadow
  edges. **It is never recommended**, only chosen: the picture follows the window, so its cost grows with the
  monitor, not with the card. A 16 GB card on a 3840x2160 monitor would go from High's 1920x1080 scene to a 3840x2160
  one, about four times the pixels, and the video memory says nothing about that. The most the recommendation gives is
  High, the game as shipped.
- **Only what costs GPU time measurably here.** On the bench ([Measurements](#measurements)) the scene's share, the
  picture's height and the stand-in's filter moved the GPU time; anisotropic filtering, texture detail, foliage,
  anti-aliasing and the soft-shadow settings stayed within the noise. So Medium keeps the shipped filtering and
  foliage, and Low lowers them only on an integrated GPU, a software renderer or a handheld, whose memory bandwidth
  may still gain. A card
  of its own that falls to Low for lacking mesh shaders or video memory (a GTX 1080 Ti, an RX 580 8 GB, a GTX 1650)
  keeps 16x filtering and full foliage, which cost it nothing measurable. `shadow_res` has no effect on 1.1.1 (the
  start logs `cfg: SHADOW_RES has no effect on this game version`) and is in no tier.
- **No tier turns dynamic resolution on** (`dyn_res`). It changes the picture's sharpness from moment to moment,
  which is the player's choice to make; a tier's `scene_share` is the share it steps down from when the player turns
  it on ([below](#dynamic-resolution)).
- **No tier sets `texture_detail`.** It cost nothing measurable on the bench, and its `auto` already picks smaller
  textures by the budget the driver grants, which knows more than a tier does.

## Which tier this PC gets

`preset_pick`, in this order:

| What the probe found | Tier | The log's reason |
|---|---|---|
| A handheld (Steam Deck, ROG Ally / Ally X, Legion Go, MSI Claw) | Low | `a handheld PC (<name>)` |
| Nothing known of the GPU (no usable Vulkan 1.3 device, or no probe and no earlier result kept) | High, then the caps below | `nothing is known of the GPU: the game as shipped` |
| An integrated GPU | Low | `an integrated GPU` |
| A software renderer on the processor (a Vulkan device of the CPU type, such as llvmpipe), with an integrated GPU's values (scene 0.4, `aniso` 4, `foliage` reduced, 30 fps) | Low | `a software renderer on the processor` |
| A GPU without mesh shaders (the renderer then emulates them, `mesh_emulation`) | Low | `the GPU has no mesh shaders, which the game's renderer then emulates` |
| Video memory under 6 GiB | Low | `<n> GiB of video memory` |
| 6 or 7 GiB | Medium | as above |
| 8 GiB or more | High | as above |

Ultra is never recommended ([above](#what-each-tier-sets)).

Then the caps, which only lower the tier: RAM under 10240 MiB (an 8 GB PC) gives at most Low
(`; <n> GiB of RAM: at most low`), under 14336 MiB (a 12 GB PC) at most Medium (`; <n> GiB of RAM: at most medium`),
and 4 logical processors or fewer at most Medium (`; <n> logical processors: at most medium`).

Video memory is compared as the size the card is sold with: an 8 GB card reports 7.8 to 8.0 GiB of device-local
memory, so it is rounded to the nearest GiB first (`preset_gib`). RAM goes by thresholds below the sold sizes instead,
because a PC with integrated graphics hands part of its RAM to them: a 16 GB PC with a Ryzen APU may report 15.3 GiB,
and still is no 12 GB PC. The thresholds are macros at the top of the header (`PRESET_VRAM_LOW_GIB`,
`PRESET_VRAM_MEDIUM_GIB`, `PRESET_RAM_LOW_MIB`, `PRESET_RAM_MEDIUM_MIB`, `PRESET_CPU_MEDIUM_THREADS`).

Examples, `tests/preset_check.py` checks each:

| PC | Tier | Why (the log's reason) |
|---|---|---|
| The Linux bench: AMD Radeon RX 6700 10 GB, 16 threads, 31 GiB of RAM | High | `10 GiB of video memory` |
| The owner's laptop (Windows): NVIDIA GeForce RTX 3050 6GB Laptop GPU (6017 MiB), 16087 MiB of RAM | Medium, with DLSS quality | `6 GiB of video memory`; its own `dlaa`, `dlss` and `taa` lines keep their values |
| A player's PC: AMD Radeon RX 6650 XT 8 GB, Ryzen 5 4650G (6 cores, 12 threads), 16 GB of RAM | High | `8 GiB of video memory`; 15.3 GiB of RAM is no cap |
| NVIDIA GeForce RTX 4090 24 GB, 32 threads, 64 GB | High; Ultra available by choice | `24 GiB of video memory` |
| NVIDIA GeForce RTX 4060 8 GB with 12 GB of RAM | Medium, with DLSS quality | `8 GiB of video memory; 12 GiB of RAM: at most medium` |
| NVIDIA GeForce GTX 1650 4 GB | Low | `4 GiB of video memory` |
| NVIDIA GeForce GTX 1060 6 GB | Low | `the GPU has no mesh shaders, which the game's renderer then emulates` |
| NVIDIA GeForce GTX 1080 Ti 11 GB | Low, keeping 16x filtering and full foliage | as above |
| Steam Deck | Low (the device profile's 40 fps) | `a handheld PC (Steam Deck)` |
| ASUS ROG Ally (Radeon 780M) | Low | `a handheld PC (ROG Ally)` |
| A laptop with Intel Iris Xe | Low | `an integrated GPU` |
| Nothing known of the GPU (no Vulkan 1.3 device, or no probe and no state file) | High | `nothing is known of the GPU: the game as shipped` |

`game_res = window` (Ultra) is never below High's 1440 lines: `widescreen.c` `picture_height` takes the window's
height with the game's 1440 as its floor, so on a 1080p monitor Ultra keeps the 1440-line picture and only draws the
scene at its full size.

The handheld comes from the firmware's strings (`preset_handheld`): Linux's `/sys/class/dmi/id` (`sys_vendor`,
`product_name`, `product_family`, `board_vendor`, `board_name`) or Windows' copy of them in the registry
(`HKLM\HARDWARE\DESCRIPTION\System\BIOS`), and `SteamDeck=1`, which Steam sets on the Deck. A handheld is Low whatever
its GPU, because its power budget, not its parts, is what limits it.

The bench PC (RX 6700 with 10 GB, 16 logical processors, 32 GB) is High: the game as shipped.

## Recommended and chosen tiers

The recommended tier is decided anew at every start. What the start does with it (`runtime/cfg.c` `preset_init`):

| `preset` in the environment or `nx2saria.ini` | This start |
|---|---|
| absent or `recommended` | the recommended tier: a first start and a start on new hardware get theirs |
| a tier, in the environment (`NX2SARIA_PRESET`) | that tier, on any hardware: the new-hardware rule below is for the settings file's line, and nothing is recorded as chosen |
| a tier, chosen on this hardware | that tier; when it is not the recommended one the log and the menu name the recommended one, as a suggestion only |
| a tier, chosen on other hardware | the recommended tier, until the player chooses a preset in the menu again |
| a tier, with no record of where it was chosen (written by hand, or the state file deleted) | that tier, and this PC is recorded as the one it was chosen on |
| anything else | the recommended tier, and the log names the value |
| any, in a run that replays a bug capture | High ([Bug captures](#bug-captures)) |

"Other hardware" compares fingerprints (`preset_fingerprint`): `vendor:device:driver:video GiB:threads:RAM GiB:handheld`,
the IDs in hex, both sizes rounded to the GiB as sold (`preset_gib`). Every field but the driver version counts
(`preset_same_hardware`), so a driver update is not new hardware, while another card, more RAM or another processor
is. Video memory counts in whole GiB because resizable BAR (Smart Access Memory) turned on or off, or a driver update,
moves the heap the card reports by a few hundred MiB. A fingerprint of the earlier form, video memory in MiB (a number
256 or more there), is compared by its GiB, so a state file from before still matches its PC. A tier chosen while nothing
was known of the GPU is recorded at the first start that knows it.

While a chosen tier gives way to the recommended one, `cfg_preset_hw_changed` tells the menu, and every start says so
in the log until the player chooses again. The player's `preset` line stays as it was: the hardware change does not
rewrite the file. Choosing in the menu (the same tier or Recommended included) and applying records this PC
(`cfg_preset_chosen`).

Ctrl+F5 follows the file: a changed `preset` line switches the layer at once (`preset_follow`), with the restart-only
keys waiting for a restart as any other change does. An edited line is the player's choice on this PC, as one made in
the menu: it ends the new-hardware state, and a tier is recorded as chosen here.

## Rendering the 3D scene below the picture's size

In this game version (1.1.1), `render_scale` and `output_res` have no effect (`nx2saria.ini.example`); the Steam Deck
profile still sets them, so on the Deck it is the Low preset that lowers the scene. The sizes come from three settings
(`runtime/widescreen.c`):

- **`game_res`**, the picture's height (1080, 1440 the game's own, 1800, 2160, 2880 or `window`, which is never below
  1440; with `game_aspect`, its shape). The HUD, the menus and the text are drawn at this size, so they stay sharp
  whatever the scene's size. Read at start.
- **`scene_share`**, the 3D scene's share of the picture's height while neither DLSS nor FSR runs (`plain_share`):
  0.75 shipped, 1 the picture's full size (the Upscaling row's *Native (100%)*), 0 the game's own dynamic resolution
  (down to a third of the picture under load). Live. With `dyn_res = 1` the scene goes below it under load
  ([Dynamic resolution](#dynamic-resolution)).
- **DLSS, FSR 3.1 or FSR 4** (`dlaa = 1`): the mode's ratio replaces `scene_share` (DLAA 1, quality 2/3, balanced
  0.58, performance 1/2, ultra performance 1/3; `mode_share`), and the upscaler enlarges the scene to the picture.

The chain, from the scene to the window:

```
3D scene --(stand-in: standin_filter = easu | bilinear)--> picture (game_res; HUD and menus drawn here)
         --(present scaler: upscaler = fsr | bilinear)--> window
```

1. **The stand-in** for the game's own upscaler (`standin_filter`, `runtime/vk_tonemap.c`) enlarges a scene smaller than
   the picture to the picture's size: `easu` (the default) with an edge-adaptive filter, `bilinear` with the plain one.
   With DLSS or FSR running it does nothing; at `scene_share = 1` it only decodes the scene, texel for texel.
2. **The present scaler** (`upscaler = fsr` with `sharpness`, or `bilinear`; `runtime/vk_fsr.c`) fits the picture to the
   window when their sizes differ.

So a 1080-line picture at `scene_share = 0.5` in a 1440-line window draws a 540-line scene, enlarges it to 1080 lines
with the stand-in, draws the HUD over it at 1080 lines and fits that to the window. The log says which way the scene
goes at each change: `widescreen: scene <w>x<h>, picture <w>x<h> (<n>% of it): <how>`.

A 1080-line picture is the one height below the game's own; the game then measures the scene against that picture
instead of against 1440 lines (`scale_k`), which `wide_init` accounts for, so `scene_share` is the share of the picture
the player chose at every height.

**Growth waits for a restart.** The game's render-target heap is sized once, at the start, for the largest scene the
settings ask for then: at least 0.75, more when `scene_share` or the upscaler's mode at start asks for more, never more
than the application pool can take (`heap_share`). A smaller share applies at once; a larger one than the start's is
held at the start's size until the next start. The menu's row says *after restart*, and the log says
`widescreen: scene_share asks for <n>% of the picture; the game's render-target heap was sized at the start for less: the next start gives it`
or, for an upscaler's mode, `widescreen: the scene is <n>% of the picture, not the mode's <n>%: ...`. Low and Medium
stay below 0.75, so switching between them and High is live; Ultra's 1 needs a restart unless the run started with it.

The menu's *Scene resolution* row (`row_scene_res`, below *Upscaling*) sets `scene_share`: 33, 40, 50, 58, 67, 75 %
(*(game)*) and 100 % (*(native)*); the entry for "unset" is named after where its share comes from, *(game)* or
*(preset)*, and takes the place of the fixed share it equals. A share set by hand that is none of these is shown as it
is. The row is shown only while neither DLSS nor FSR is chosen, since their mode decides the scene's size then.

### Dynamic resolution

`dyn_res = 1` (off by default; no tier turns it on) lets the scene's size follow the GPU's time per frame toward a
target, one step at a time (`runtime/dynres.c`). The HUD and the text stay at the picture's size; only the scene
changes. The menu has it in *Settings > Screen > Picture*, below *Scene resolution* (`runtime/ui/menu.cpp`
`row_dyn_res`): *Dynamic resolution* (on / off), shown with and without an upscaler, and *Minimum scene resolution*
(`dyn_res_min`, the Scene resolution row's shares from 33 % to 100 %), greyed out while *Dynamic resolution* is off and
hidden while DLSS or FSR runs, where it does nothing. Both are live. `dyn_res_target` and `dyn_res_step` stay in
`nx2saria.ini` only.

The target is `dyn_res_target` milliseconds when that is set (1 to 100), exactly. Unset (0) it is 90 % of the frame
time of the rate the game draws at under the limit, or 16.7 ms while that rate is unknown or unlimited: 15.0 ms at
60 fps, 22.5 ms at 40, 30.0 ms at 30. A step down comes at 108 % of the target (below), so with the 10 % margin the
scene shrinks at 97 % of the frame time, before frames are lost, and grows back under 67.5 % of it.

What it steps depends on what enlarges the scene:

- **DLSS or FSR 3.1 running** (`upscaler_runs`): the upscaler's mode, from the `dlss` mode set down to performance.
  It needs an upscaling mode; with DLAA or FSR at native size it does nothing and the log says so.
- **Neither** (the stand-in's `easu` or `bilinear`): `scene_share` itself, from the share in effect (the one set, the
  tier's, or 0.75; within the heap the start made) down to a floor, each step 0.87 of the one above on each axis, about
  three quarters of the pixels (0.75, 0.65, 0.57, 0.50 with the defaults). The floor is the lower of that share and
  `dyn_res_min` (0.33 to 1, default 0.5): it never goes above the share the player or the tier chose, nor below the
  floor. A share at or under `dyn_res_min` leaves it no room: Low's 0.4 on an integrated GPU against the default 0.5
  stays at 0.4, and the log says so once. A changed `scene_share` or `dyn_res_min` starts again at the new share.
  `scene_share = 0` (the game's own dynamic resolution) has nothing to step.

The steps hold still on purpose, with times fixed in the code: a step down when the GPU's average is 8 % over the
target, a step up when it is under 75 % of it (a step up takes about 1 / 0.757 more pixels, so it lands near 99 % of
the target, under the 8 % that would send it down again), at least 1 s between two steps whatever `dyn_res_step` says
(60ths of a second, default 45), and at least 3 s from a step down to a step up. Where one step moves the GPU time by
more than the gap between the two thresholds, a step up that a step down undoes within 5 s doubles that wait, up to
48 s, so the scene does not swing between two sizes every few seconds; a step up that holds 5 s brings it back to 3 s.

Its log lines, at the start, at a step, every 600 frames, when the limit moves the target and when it has no room:

```
resolution: dynamic resolution on (the scene share, no upscaler runs): target 15.0 ms (the frame limit's 16.7 ms less 10 %), steps at least 1.00 s apart and 3 s from a step down to a step up, scene share 0.75 down to 0.50
resolution: dynamic resolution: GPU 17.40 ms a frame against 15.00 ms: scene share 0.75 -> 0.65
resolution: dynamic resolution: GPU 13.10 ms a frame, scene share 0.65, target 15.00 ms
resolution: dynamic resolution: target 7.50 ms (the frame limit's 8.33 ms less 10 %)
resolution: dynamic resolution has no room: the scene share 0.40 is at or under dyn_res_min 0.50, the lowest it may go; the scene stays at 0.40
```

The step lines stop after 200 in a run (one line says so), the summaries after 12, and the scene's own
`widescreen: scene ...` line follows the first 40 steps.

## The hardware probe

The preset has to be known before any restart-only setting is read (the application pool's size goes by the resolution
settings a preset sets), long before the game makes its Vulkan device. So `runtime/hw_detect.c` `hw_detect` runs a
probe of its own:

- a Vulkan 1.3 instance without surface extensions, which lists the devices and picks the one `runtime/vk_device.c`
  `pick_device` would pick: `gpu_index` or `gpu_name`, else a discrete GPU before an integrated one before any other,
  among those with Vulkan 1.3 and the extensions the game needs; of one GPU listed by two drivers, the newer driver's
  listing. It reads the GPU's name, IDs, driver version, type, largest device-local heap and mesh shader support (mesh
  and task shaders both), and is destroyed again;
- the processor's logical processors and physical cores, the installed memory (`runtime/platform/`), and the handheld.

**When it runs.** `runtime/cfg.c` `cfg_start`, called from `runtime/memory.c` `mem_init` once the fixed reservations
are made (the fixed window, the stack and heap block, the alias region), so the drivers' libraries and allocations
land outside the guest's address space, as the game's own device's do later; before the application pool is sized,
and before `start_values` snapshots the restart-only keys, since a preset's are among them. The probe is the process's
first Vulkan call, so it puts the drivers' cache variables in the environment first (`vk_driver_cache_env`). Its time,
the drivers' first load included, goes to the log at every start (`probe <ms> ms`).

**When it does not.** In a tool mode (any command line but the game's own options, `runtime/main.c` `game_run`); with
`preset_probe = 0` (`NX2SARIA_PRESET_PROBE`, read at start, no menu row); after a start whose probe never returned
(below); on Windows with `framegen = dlss` (Streamline's interposer must make the first Vulkan call) and with a
RenderDoc capture set up (`rdoc`). The start then goes by the last probe's GPU, which the state file keeps; without
one, nothing is known of the GPU, which gives High, the game as shipped.

**A probe that never returns.** The probe runs inside `mem_init`, the first thing `runtime/main.c` `main_run` does:
before the crash report is set up (`plat_crash_hook`) and before the log file opens (`log_open`). A driver that
crashes or hangs while the probe makes its instance would end such a start with no report and no log file, and the
next start would do the same. So right before its first Vulkan call the probe logs
`preset: probing the GPU (a Vulkan instance; preset_probe = 0 turns it off)` and the game writes `probing = 1` into
the state file (`runtime/cfg.c` `preset_probe_begin`, through the same temporary file and rename); once the probe has
returned, the start writes the file without it. A start that finds the line skips the probe and goes by the last
result, and says so in its log. The line goes once that start's game has chosen its own Vulkan device
(`runtime/cfg.c` `cfg_preset_device_started`, called by `hw_detect_device_check`): it has then made an instance and
listed the devices, as far as the probe goes, so the drivers load, and the next start probes again. A driver that
crashes again in the game's own start leaves the line, and every later start goes without the probe until the player
deletes the line (or the file). The same holds when the write that takes the line out fails (an antivirus holding the
file): one start goes by the last result, and its own device takes the line out. Lines logged before `log_open` wait in a buffer that `log_open` writes to
the file, and reach stderr at once, so the line before the probe is on stderr even when the start ends inside it.

**The game's device afterwards.** `pick_device` compares the device the game chose with the probe's and logs the
result (`hw_detect_device_check`). The window's present support cannot be asked without a window, so a GPU the game
refuses for that is the one case where the two differ.

## The state file

`preset-state.txt`, beside the game executable and `nx2saria.ini`: the game's record of what it saw, never settings.
It is written through a temporary file and a rename, only when its text changes, and only by the game itself: not by a
tool mode, not in a run that replays a bug capture. Lines `key = value`:

| Key | What |
|---|---|
| `hardware` | this start's fingerprint |
| `chosen_on` | the fingerprint when the player last chose a preset; absent while none is recorded |
| `noticed` | the recommended tier the one-time notice last named ([The settings menu](#the-settings-menu)); absent before any |
| `gpu_name`, `gpu_vendor`, `gpu_device`, `gpu_driver`, `gpu_type`, `gpu_vram_mib`, `gpu_mesh` | the last probe that found a GPU, for a start that cannot probe; a GPU missing any of these lines, or with a value of the wrong kind, counts as none |
| `probing` | `1` while a probe runs; a start that finds it does not probe, and takes it out once the game's Vulkan device is chosen ([above](#the-hardware-probe)); absent otherwise |

```
# NX2Saria: what the quality presets last saw of this PC. The game writes this file; the settings are in nx2saria.ini.
hardware = 1002:73df:1:10:16:31:none
chosen_on = 1002:73df:1:10:16:31:none
gpu_name = AMD Radeon RX 6700 (RADV NAVI22)
gpu_vendor = 0x1002
gpu_device = 0x73df
gpu_driver = 0x1
gpu_type = discrete
gpu_vram_mib = 10224
gpu_mesh = 1
```

Deleting it is safe: the next start probes again, a tier in `nx2saria.ini` is then taken as chosen on this PC, and a
notice that is due shows again. The file is read to its end (blank lines included); a line that cannot be read as
these is skipped. Every start that probes writes the file twice, with the `probing` line and without it; when nothing
else changed, it ends as it was, and the log says `preset: wrote` only for a change.

## The settings menu

The *Quality preset* row is the first of *Settings > Screen > Picture* (`row_preset`): *Recommended (…)*, *Low*,
*Medium*, *High*, *Ultra*; *… (custom)* while keys the tier sets were set by hand to other values. Its note is
*New hardware: the recommended preset is in use* while a chosen tier gives way on new hardware, *set by hand: …*
when custom, or *Recommended for this PC: …* when a chosen tier is not the recommended one.

- **Choosing is part of the draft** ([Applying changes in the menu](controls.md#applying-changes-in-the-menu)). The
  row stages `preset` (`""` for Recommended) and hands back to the preset every key the chosen tier sets that the
  player set by hand to another value than the tier gives it (`opt_preset_handback`, `cfg_preset_differs`), staged as
  unset, so the whole tier applies. A key the chosen tier leaves alone, or one set to the tier's own value, stays the
  player's line. The other rows show the staged tier's values at once (`cfg_default_tier`; the *Resolution* row's
  "default" mark too, `opt_default`), but the layer itself switches only at Apply (`is_staged`).
- **Keys set by hand ask first.** When there are such keys, a box (`preset_confirm`) says *These settings were changed
  by hand. The preset sets them to its own values:* and lists `<row>: <value now> -> <the tier's value>`; *Use the
  preset* stages the choice, *Cancel* (the default, Esc or B) stages nothing. Keys the environment sets are never
  handed back.
- **Recommended on new hardware.** While a tier chosen on other hardware gives way, the row shows *Recommended (…)*,
  which is in use, and choosing it is still a change: left on it (or a click on its left arrow) stages Recommended,
  and the draft's line says `<the file's tier> -> Recommended (…)` (`opt_choice_index`'s `again`). Apply then removes
  the `preset` line and records this PC (`cfg_preset_chosen`), as for any other choice.
- **Apply** (`option_rows.cpp` `opt_draft_apply`) writes the draft: `preset = <tier>` in `nx2saria.ini` (no line for
  Recommended), the handed-back keys' lines removed. It then switches the layer (`cfg_preset_use_from`, which logs each
  value that changes from what the player saw before the draft) and records this PC as the one the preset was chosen
  on (`cfg_preset_chosen`). The restart-only keys a tier sets (`game_res`, `shadow_pcf`, `foliage`) wait for a
  restart: the Apply box marks the preset line *after restart* when one of them
  changes, and the restart box follows as for any other.
- **The one-time notice** (`preset_notice_tick`): a player who never set `preset`, on a PC whose recommended tier is
  not High, sees *Quality preset Recommended (…) is in use for this PC (Settings > Screen). For the game as before, choose High
  there.* once, for 8 seconds, at the first
  frame the game runs with the menu closed and no other notice up. The state file's `noticed` keeps it shown (written
  by a thread of its own, `state_write_later`, so the present thread does not wait for the disk); it comes again only
  when the recommended tier changes (new hardware). Choosing a preset in the menu, Recommended included, counts as
  having seen it. Never in a tool mode or a replay.
- *Scene resolution* ([above](#rendering-the-3d-scene-below-the-pictures-size)).
- The full performance overlay names the preset in use, with *(custom)* when it is.

## Bug captures

A capture ([bug-capture.md](bug-capture.md)) carries the preset twice:

- `capture.txt`, section `[system]`: `quality_preset = <in use> (recommended <tier>: <reason>)`.
- `settings.ini`: after the player's settings, the comment line
  `# The values the quality preset (<tier>) gave the keys above leave unset, for a replay, which uses none:` and a
  `<key> = <value>` line for every key a preset can set that the player left unset and whose value in effect differs
  from the table's default (the preset's or the device profile's). A replay runs on High, which sets nothing, and reads
  these lines as settings: the replaying PC's own preset would otherwise give these keys the values of another PC.

## The log

All in `logs/nx2saria.log`, at the start unless said otherwise (formats from `runtime/cfg.c`, `runtime/hw_detect.c`
and `runtime/ui/`):

```
preset: probing the GPU (a Vulkan instance; preset_probe = 0 turns it off)
preset: hardware: GPU "<name>" (vendor 0x<id> device 0x<id>, <discrete|integrated|cpu|other>, <n> MiB video memory, mesh shaders <yes|no>, DLSS <yes|no>, FSR 4 <yes|no>); CPU <n> cores / <n> threads; RAM <x.x> GiB; handheld <name|none>; probe <ms> ms
preset: recommended <tier>: <reason>
preset: in use <tier> (<source>)
preset:   <KEY> = <value> (<where it comes from>)
preset: wrote <folder>/preset-state.txt
```

- The hardware line without a GPU: `GPU none (<why>)`, the why one of `no Vulkan 1.3 instance (<code>)`,
  `no Vulkan device`, `no device with Vulkan 1.3 and the extensions the game needs` or
  `not probed, and no earlier probe kept`. Without a probe its end is `probe skipped: <why>` (`a tool mode, not the
  game itself`, `preset_probe = 0`, `the last start's probe did not finish (preset-state.txt says probing = 1);
  delete that line to probe again`, `DLSS Frame Generation (framegen = dlss) makes the process's first Vulkan call`,
  `a RenderDoc capture (rdoc) is set up`), with `; the GPU as the last probe saw it` when the state file had one. The
  probing line comes only when the probe runs.
- While the state file holds the probing mark, after the per-key lines: `preset: the probe of an earlier start did not
  finish (a driver that crashed or hung in it, or a write that failed): no probe this start; the mark goes once the
  game's Vulkan device is chosen, or delete the line 'probing = 1' from <path>`; then, when the game has chosen its
  device: `preset: the game's Vulkan device is chosen: the probing mark of an earlier start is taken out of <path>, and
  the next start probes again`.
- `<source>`: `recommended`, `environment` (also on new hardware), `settings file`,
  `recommended: the hardware changed since <tier> was chosen`, or
  `a bug capture's replay: its settings.ini holds the capture's preset values`.
- After it, one of: `preset: <tier> chosen; the recommended for this PC is <tier>`;
  `preset: <tier> was chosen on <fingerprint>, and this PC is <fingerprint> (vendor:device:driver:video GiB:threads:RAM GiB:handheld); choosing a preset in the settings menu keeps it again`.
  Before it, for a value that is no tier: `preset: '<value>' is no preset: recommended`.
- One line per key the tier sets: `(preset)`; `(settings file, the same as the preset)` or
  `(environment, the same as the preset)`; `(settings file; the preset's <value> stays unused)` or
  `(environment; the preset's <value> stays unused)`; `(device profile)` or
  `(device profile; the preset's <value> stays unused)`. A tier that sets nothing (High) logs
  `preset: high sets nothing: the game as shipped` instead.
- `preset: cannot write <path>` when the state file cannot be written.
- On a Steam Deck, after the recommendation: `preset: Steam Deck: the GPU clock is left to the system's power management
  (no peak clock request)`. That is where asking the amdgpu driver for its stable peak clock would go (it raises the
  Deck's frame rate at more power); it waits until it can be measured on a Deck.
- When the game's Vulkan device is chosen: `preset: the Vulkan device matches the probe` (with
  `; its driver is 0x<v>, the probe's result was from 0x<v>` when the state file's result is older), or
  `preset: the Vulkan device differs from the probe: the game uses "<name>" (...), the preset was chosen for "<name>" (...)`,
  or `preset: the Vulkan device is "<name>"; the preset had no GPU to go by`.
- In play: `preset: <old> -> <new>: <KEY> <before> -> <after>` for each value a switch changes (or
  `preset: <old> -> <new>: no setting changes`), `preset: the settings file now says <tier>` after Ctrl+F5, and
  `preset: notice shown: recommended <tier> in use` for the one-time notice.
- The menu: `menu: preset: <tier> chosen in the draft; <n> key(s) set by hand handed back to it`,
  `menu: box "choose this preset?" opened: <n> key(s) set by hand` with `: yes` or `: Cancel (nothing staged)`, and at
  Apply `menu: draft: quality preset <tier> in use`.

## Tests

`python tests/preset_check.py` builds `tests/preset_check.c` with `runtime/cfg.c` and stand-ins for what `cfg.c` links
against (`tests/preset_check_stubs.h`; the probe's among them: the made-up hardware comes in `NX2SARIA_TEST_HW`, and
`crash=1` ends the process inside the probe), and checks with no GPU and no game files:

- `pure`: the tier each made-up PC gets (the bench PC is High, an RTX 4090 too, and no PC of a wide sweep Ultra), the
  handheld names of both firmware sources, and what each tier sets: High nothing, no tier frame generation, DLSS and
  FSR 4 only below High and only on the cards that run them, a frame-rate limit only on an integrated GPU or a
  handheld and never above 60, filtering and foliage lowered only there, no `texture_detail`; fingerprints (a heap
  256 MiB off, the older form in MiB);
- `values`: every value of the table, read by `cfg.c` as a settings-file line and as a Ctrl+F5 reload: none outside
  its setting's choices;
- `run`: `cfg_init` and `cfg_start` with a settings file and a state file: the layer's order (an ordinary setting in
  the environment over the preset; the device profile over a tier for one key, through a profile row the test build
  alone has, `CFG_TEST_DECK_ROW`; `cfg_default` on the Deck: profile, then tier, then table), the log lines (the
  hardware line against a pattern), the state file (written only when it changes, never `nx2saria.ini`; blank lines;
  a GPU missing a line), a tier chosen on other hardware, a tier in the environment on other hardware, a driver update
  and a smaller heap, a start that cannot probe, `preset_probe = 0`, a probe that crashes and the starts after it, a
  tool mode, a state file of garbage, the menu's calls (`cfg_preset_use`, `cfg_preset_use_from`,
  `cfg_preset_chosen`, `cfg_preset_custom`, `cfg_default_tier`, `cfg_preset_differs`), the one-time notice and a
  reload of the `preset` line, also on new hardware;
- `tests/preset_draft_check.cpp`, built with `runtime/ui/option_rows.cpp`, `runtime/ui_text.c`, `cfg.c` and Dear ImGui
  (nothing is drawn): the draft while a tier is staged (`opt_value`, `opt_get`, `opt_default`, `opt_is_set`), the
  handback of each tier (`opt_preset_handback`; the owner's laptop below), Apply and Discard, the Deck's profile under
  a staged tier, and Recommended chosen on new hardware.

It is listed with the other checks in [building.md](building.md#checks). The menu's rows can be looked at with
`tools/menu_preview.py` (same page).

## Recalibrating

The numbers are meant to move with measurements:

- **What a tier sets**: edit `PRESET_ROWS`. Keep a row's value one of its setting's choices (the `values` test
  refuses another), keep High without rows and no row on `framegen`, and put a row for a narrower kind of hardware after
  the general one for the same key (`PRESET_SMALL` after `PRESET_ANY`). A new key needs nothing else in `cfg.c`, which
  collects the keys from the table, up to 32 (`PRESET_KEYS_MAX`); give it a name in the menu's confirmation box
  (`runtime/ui/menu.cpp` `preset_key_label`) if a row shows it.
- **Where the tiers start**: the `PRESET_VRAM_*`, `PRESET_RAM_*` and `PRESET_CPU_MEDIUM_THREADS` macros, or
  `preset_pick` for another kind of rule. Update the expected tiers in `tests/preset_check.c` `pure` with it.
- **Which cards run DLSS or FSR 4**: `preset_gpu_caps`. A new handheld: `preset_handheld`, with its firmware strings in
  the `pure` test.
- Measure before and after with `tools/bench.py` ([performance.md](performance.md)) and `--set NX2SARIA_PRESET=<tier>`
  (or `--set NX2SARIA_SCENE_SHARE=<share>` for one share). The bench sets `NX2SARIA_FPS_LIMIT=unlimited` in the
  environment, which wins over a tier's limit, as it should for a measurement. Record the numbers below, with the
  commit, the scenes and the bench's mode.

## Measurements

Bench machine: AMD Radeon RX 6700 (10 GB, RADV, Mesa 25.2.8), an Intel engineering-sample CPU with 16 logical
processors, 32 GB of RAM, Linux; it is recommended High. `tools/bench.py run ... --boot-map --runs 2`, the scenes
`kokiri` and `field` of `tools/bench/scenes.toml`, a 1920x1080 offscreen window, frame limit off. GPU ms is the GPU's
busy time per frame from the timestamp queries (the bench's `gpu_ms`); between two runs of one configuration it varies
by up to about 1 ms and the average frame rate by up to about 1.5 fps.

**This machine is CPU-bound** (about 32 fps; the runtime's verdict was `cpu` in every 60-frame window), so the frame
rate hardly moves and the GPU time is what tells the settings apart. The gains below are a floor: on a PC whose GPU is
the limit, the same GPU time saved is frame rate gained. A run on a GPU-bound PC (below) calibrates them.

### What each setting costs (kokiri, the game as shipped = 20.2 ms of GPU time, 31.7 fps)

| Setting | GPU ms against shipped | Avg fps | Scene / picture (log) |
|---|---|---|---|
| `scene_share = 1` | +2.2 | 29.3 | 2560x1440 / 2560x1440 |
| `scene_share = 0.67` | -0.5 | 32.0 | 1715x965 / 2560x1440 |
| `scene_share = 0.5` | -0.9 | 32.0 | 1280x720 / 2560x1440 |
| `scene_share = 0.4` | -2.3 | 34.8 | 1024x576 / 2560x1440 |
| `scene_share = 0.33` | -2.2 | 34.7 | 845x475 / 2560x1440 |
| `game_res = 1080` | -1.6 | 34.3 | 1440x810 / 1920x1080 |
| `game_res = 1080`, `scene_share = 0.5` | -2.2 | 34.6 | 960x540 / 1920x1080 |
| `game_res = 2160` | +2.6 | 29.8 | 2880x1620 / 3840x2160 |
| `standin_filter = bilinear`, `scene_share = 0.5` | -1.9 (-1.0 against easu at 0.5) | 34.3 | 1280x720 / 2560x1440 |
| FSR 3.1 quality (`dlaa = 1`, `taa = fsr3`) | +2.9 | 28.1 | 1707x960 / 2560x1440 |
| FSR 3.1 performance | +1.5 | 29.5 | 1280x720 / 2560x1440 |
| `shadow_res` 512 or 2048, `shadow_pcf`, `pcss`, `contact_shadows`, `aniso = 4`, `texture_detail` full or low, `foliage = reduced`, `aa` fxaa or off | within the noise | 32.0 to 33.0 | as shipped |

`shadow_res` logs `cfg: SHADOW_RES has no effect on this game version`. FSR 3.1 costs more than the plain path even
with the smaller scene, which is why no tier uses it on cards without DLSS or FSR 4. Pictures at `scene_share` 0.33,
0.5 and shipped show the HUD equally sharp in all three (it is drawn at the 2560x1440 picture); at 0.33 the scene is
visibly softer and nothing breaks.

### The tiers

Each tier forced with `NX2SARIA_PRESET` on a build of this branch, an empty `nx2saria.ini`, 2 runs per row; the
scene and picture sizes are the ones the log reports (`widescreen: scene ...`). GPU ms against High in brackets.

| Tier | Scene / picture (log) | kokiri: avg fps, 1% low, GPU ms | field: avg fps, 1% low, GPU ms |
|---|---|---|---|
| Ultra | 2560x1440 / 2560x1440 (100 %), picture kept at 1440 in the 1080p window | 30.1, 16.7, 21.9 (+1.8) | 29.9, 17.4, 22.5 (+1.4) |
| High = Recommended here (the game as shipped) | 1920x1080 / 2560x1440 (75 %) | 31.3, 16.1, 20.1 | 30.9, 16.5, 21.1 |
| Medium | 1286x724 / 1920x1080 (67 %) | 32.2, 18.2, 18.8 (-1.3) | 33.5, 18.1, 19.1 (-2.0) |
| Low | 960x540 / 1920x1080 (50 %), bilinear | 34.0, 15.8, 17.3 (-2.8) | 34.0, 21.0, 18.3 (-2.8) |
| Low with `scene_share = 0.4` (as on an integrated GPU or handheld) | 768x432 / 1920x1080 (40 %), bilinear | 35.5, 20.6, 16.8 (-3.3) | — |

The Recommended run with nothing set (32.3 fps, 20.0 GPU ms) logged `preset: recommended high: 10 GiB of video memory`,
`probe 28.7 ms` and `preset: the Vulkan device matches the probe`, and every run left `nx2saria.ini` as it was. A
picture of Low in kokiri shows the HUD as sharp as at High: it is drawn at the 1920x1080 picture, the window's own
size, and only the 960x540 scene is softer.

### Dynamic resolution (kokiri, target 8 ms)

One run each, `dyn_res_target = 8` so that the controller steps on this machine (its GPU never gets near a 16.7 ms
target while the CPU holds it near 32 fps). The steps all went down, each held, and none came back up while the time
stayed over the target:

| Configuration | Steps the log shows | GPU ms | Avg fps |
|---|---|---|---|
| High, `dyn_res` off | — (scene 1920x1080 of 2560x1440) | 18.1 | 30.8 |
| High, `dyn_res = 1` | 0.75 -> 0.65 -> 0.57 -> 0.50 (scene 1280x720) | 17.0 | 32.7 |
| Medium, `dyn_res` off | — (scene 1286x724 of 1920x1080) | 18.0 | 32.5 |
| Medium, `dyn_res = 1` | 0.67 -> 0.58 -> 0.50 (scene 960x540) | 17.7 | 32.8 |
| Low, `dyn_res = 1` | none: `has no room: the scene share 0.50 is at or under dyn_res_min 0.50` | 17.1 | 30.0 |
| Low, `dyn_res = 1`, `dyn_res_min = 0.33` | 0.50 -> 0.43 -> 0.38 -> 0.33 (scene 634x356) | 15.4 | 33.3 |
| FSR 3.1 quality, `dyn_res` off | — (scene 1707x960 of 2560x1440) | 23.4 | 26.5 |
| FSR 3.1 quality, `dyn_res = 1` | quality -> balanced -> performance (scene 1280x720) | 21.4 | 27.9 |

The scene's share moves this game's GPU time less than its pixels: the picture-sized passes after the scene and the
CPU-bound pace stay as they are, so a quarter of the pixels gives about 1 to 2 ms here. Each GPU ms is a run's
average, the time at the larger steps before the first step down included.

### On a GPU-bound PC (the owner's test)

On the owner's laptop (NVIDIA GeForce RTX 3050 6GB Laptop GPU, Windows; recommended Medium), in the same place for
each step, about a minute of play each:

1. Start with nothing changed in the Quality preset row (*Recommended (Medium)*): the log's `preset: hardware:`,
   `preset: recommended medium: 6 GiB of video memory` and `preset: in use medium (recommended)`, and the
   `frametime: 60 s: ...` lines.
2. Choose *Medium*, then *Low* (Apply, restart for the restart-only lines): the same `frametime: 60 s` lines for each,
   and the `preset: medium -> low: ...` lines.
3. Choose *High* to compare with the game as shipped.

In step 1 the laptop's own `dlaa`, `dlss` and `taa` lines win (its `taa = fsr3` runs FSR 3.1, not the tier's DLSS).
Its `dlaa = 1` and `dlss = quality` equal Medium's own values, so the box before choosing Medium in step 2 lists only
*Upscaling (taa)*: `fsr3 -> dlss`; before Low it lists `taa` and `dlss` (`quality -> performance`). *Use the preset*
hands those back, so each step runs the tier's own values (DLSS quality on Medium, DLSS performance on Low), and the
`dlaa` line stays the player's; the log's `menu: draft` lines keep the old values to set back by hand. The `frametime` lines of each step give the numbers to recalibrate the tiers with on a machine
whose GPU is the limit.
