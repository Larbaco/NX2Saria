# runtime/cfg

The settings: the table of every setting with its default and the values it takes, the settings file
(`nx2saria.ini`), the environment, the device profiles, the quality presets and the hardware probe they start from.
The player's side: [presets.md](../presets.md) for the quality presets, `nx2saria.ini.example` for every setting the
settings file takes. `runtime/cfg.h`, the interface, belongs to the runtime's top level.

The notes below follow the code: a note about one line names that line's code, a note about a table row names its keys.

## `runtime/cfg/cfg.c`

`nx2saria.ini`: file-backed defaults for NX2SARIA_* variables; `nx2navi.ini` and NX2NAVI_* remain readable aliases. Format: "key = value" lines, '#' or ';' at the start of a line begins a comment, [sections] only group. A key is a variable name without either prefix (both optional prefixes are accepted), case-insensitive. A value is the rest of the line, trimmed, optionally in double quotes; an empty value counts as unset. For a setting with a set of values (a row of metas[] with values), a blank followed by ';' or '#' starts a comment after the value too (cut_comment). A switch (values 0|1) reads false, off and no as 0 and true, on and yes as 1 (switch_word). The environment wins over the file.

Live settings: cfg_set changes a value in memory (readers see the new pointer; the old string is kept, so a pointer cfg_get handed out stays valid) and cfg_save writes only the changed keys back, keeping the rest of the file. Readers take no lock: an entry is complete before n_entries counts it, and value pointers are swapped atomically.

### Types

- `typedef struct { char *key, *value; int dirty, removed, held, test;...`: key upper-case, without the prefix; value NULL after cfg_reset; held: previewed by the menu, cfg_save skips it; test: set by a test hook (cfg_set_test), cfg_save skips it too
- `typedef struct { const char *s; size_t n; } Line;`: n includes the '\n' (and '\r') when present

Cfg_reload_file: the file's lines as a list of (key, value), a repeated key keeping its later line.

### `set_lock`

- `static PlatMutex set_lock = PLAT_MUTEX_INIT;`: cfg_set, cfg_reset, cfg_save
- `static uint64_t kept_n, kept_bytes;`: set_lock: values cfg_set replaced and kept for readers (cfg_sizes)

### `save_ok`

- `static int save_ok = 1;`: the last cfg_save_to wrote (or had nothing to write)

### The table `known[]`

- Every name the runtime reads (and the patch sets in generated/), without the prefix. A file key outside this list is logged as unknown. A new variable goes here too, or its ini key logs a harmless "unknown key" line.
- `CORES`, `TM_DISPATCH`, `NVN2_CS_TABLE`, `SKIP_SHADERS`, `MOVIE_DEBAND`, `MOVIE_FPS`, `MOVIE_SKIP`, `MOVIE_OVERLAY`, `MOVIES`, `NPLN`, `NVN2_PROGRAMS`, `NVN_TRAP`, `OCARINA_REALTIME`, `ONESHOT_BUDGET`, `ONESHOT_WAIT_MS`, `OCARINA_STEP`, `PHYSICS_STEPS`, `PHYSICS_STEPS_FULL`, `PICKUP_COUNT`, `SONG_STAFF`, `WATER_STEPS`, `SOUND_STEPS`, `FLOOR_PROBE`, `FLOOR_SCAN`, `FLOOR_WATCH`, `FLOOR_CARRY`, `MOVIE_PTS`, `STEP_STEADY`, `VI_HZ`, `VI_VRR`, `TIME_DUMP`, `SHADER_DUMP`, `SHOT_AFTER`, `SHOT_DIR`, `SHOT_EVERY`: Switch 2 port: movie skip (`romfs.c`), NVN2 proc traps (`nvn_procs.c`)
- `FSR3_PROBE_PAN`, `FSR3_PROBE_PPM`: the FSR 3.1 probe (`fsr3_probe.c`)
- `FSR3FG_PROBE_PPM`, `FSR3FG_PASS_TIMES`, `FSR3FG_DUMP`, `FSR3FG_DUMP_EVERY`, `FSR3FG_TEST_FAIL`: the FSR 3.1 frame generation probe (`fsr3fg_probe.c`), `vk_fsr3fg.c`
- `FG_GPU_TIME`, `DLAA_TEST_DEFERRED`: frame generation's GPU time (`vk_fg_time.c`); the deferred-shape test (`vk_dlaa.c` vk_dlaa_pass)
- `CAPTURE_AT`, `CAPTURE_DIR`, `CAPTURE_REPLAY`, `CAPTURE_REPLAY_CAMERA`: bug captures and their replays (`bugcap.c`)
- `BOOT_MAP`, `BOOT_MAP_SLOT`, `BOOT_MAP_AGE`, `BOOT_MAP_TIME`: the game started in an area from a save (`boot_map.c`)
- `MOTION_GUARD_DEGREES`, `MOTION_POLICY_TRACE`, `SELECTIVE_MOTION`, `SELECTIVE_MOTION_RAW`: `motion_owner.c`, `vk_fg_pose.inc`
- `SKIN_MOTION`: `vk_skin_motion.inc`
- `GYRO_CALIBRATION`, `GYRO_DEADZONE`, `GYRO_TURN_SENS`, `GYRO_TILT_SENS`, `GYRO_TURN_INVERT`, `GYRO_TILT_INVERT`, `TEST_PAD_GYRO`: `input.c` motion_options_refresh and test_pad_make (TEST_PAD_GYRO): the gyro
- `TRACKER_API`, `TRACKER_API_PORT`: the read-only view of the game's memory for a map tracker (`tracker_api.c`)
- `TEXUP_LOG_MS`, `DYN_RES`, `DYN_RES_TARGET`, `DYN_RES_STEP`, `DYN_RES_MIN`, `TEXTURE_DETAIL`, `VRAM_USE`: vram_use: the removed Video memory use row, read by nothing
- `PRESET`: the quality preset (`cfg.c` "quality presets", `preset_pick.h`)
- `PRESET_PROBE`: the quality preset's hardware probe at start (`hw_detect.c`); no menu row
- `VRAM_WARNING`: the on-screen notice of full video memory (`vram_warn.c`); no menu row
- `NVIDIA_CACHE_DIR`: NVIDIA's shader disk cache in a folder beside the exe (`vk_device.c` nvidia_cache_beside_exe); no menu row
- `TEX_COMPACT`, `TEX_COMPACT_MIN_MB`, `TEX_COMPACT_FRAME_MB`: texture compaction (`vk_texture.c`); no menu row
- `STANDIN_FILTER`: the filter of the scene upscaler's stand-in (`vk_tonemap.c`); no menu row
- `AUDIO_DUMP`, `DEFER_STAMP`, `DEPTH_TWIN`, `DLAA_ZFIT`, `DUMP_SKIP_RUNS`, `DUMP_TEX`, `DUMP_UBO`, `EVENT_ORDER`: developer switches read with cfg_get, rt_knob or `vk_caps.c` env_u32, without a menu row (`tests/cfg_rows_check.py`)
- `HEAP_RETRY`, `HEAP_RETRY_TEST_FAIL`, `HEAP_RETRY_TEST_SHIPPED`: `heap_retry.c`: the set heap_retry and its test switches
- `CHANGEBANK_HOLD`, `CHANGEBANK_HOLD_MS`: `changebank_hold.c`: the set changebank_hold and its hold
- `CAMERA_CLEARANCE_FIX`: `camera_clearance.c`: the set camera_clearance
- `POS_TRACE`: `debug_tools.c`: Link's position each frame to a file, for movement checks; no menu row
- `HANG_WATCH_S`: `hang_watch.c`: the hang watch's seconds
- `COOP`, `COOP_BETA`, `COOP_NAME`, `COOP_SERVER`, `COOP_PORT`, `COOP_ROOM`, `COOP_AUTO`, `COOP_DROP`: retired: the settings of the co-op of versions up to 0.9.2; read by nothing, listed so that a file which still sets them logs no unknown key
- `COOP_STEAM`, `COOP_SHARE`, `COOP_NAMES`, `COOP_CHAT`: retired: the settings of the co-op through Steam that the main branch held for a while after 0.9.2 and no release shipped; listed like the ones above, for the file of someone who built from it then and turned it on

### The table `sections[]`

- The settings-file section a key cfg_set may write goes to when the file lacks it (the example file's sections).
- `TEX_RELEASED_MAX`, `MIRROR_HOT`, `FREEZE_IDLE`, `SUBMIT_FINISH_KINDS`, `FOLIAGE`: the settings the low-end GPU mode had rows for up to 0.10.1 (lowend_set, below)

### The settings table `metas` (`LIVE`, `RESTART`)

The settings the menu shows: the value in effect when the key is unset, and what the key accepts. The only place a default lives: consumers read cfg_value / cfg_int / cfg_double / cfg_bool. A row with an empty default has no fixed value (it depends on the machine); its consumer reads cfg_get and keeps an "unset" branch.

### `WB_LAZY_DEFAULT`

The lazy write-back (`memory.c` "Lazy write-back") is on where it has run: its fetch inside the access-fault handler has only ever run on Windows. This is the platform's part of the default, which `memory.c` mem_init goes by to take the access faults; the back end then turns it off for a driver it is not trusted on while the key is unset (`wb_lazy_choice.h`: AMD's own), and with shadow storage whatever is set (`vk_memory.c` vk_mem_lazy_decide).

### The table `metas[]`

- *Screen*
- `GPU_NAME`: `gpu_choice.h`: the Graphics card row writes the card's name beside its position
- `PRESET`: the quality preset (`cfg.c` "quality presets"): recommended is the tier `preset_pick.h` chooses for this PC at each start; the restart-only keys of a tier the menu switches to apply at the next start (cfg_pending)
- `PRESET_PROBE`: 0: no Vulkan probe at start for the preset; the state file's last GPU, else nothing known (`cfg.c` preset_init)
- `LOW_LATENCY`: `vk_lowlat.c` level_now: the driver's low latency without frame generation (0 off, 1 on, 2 on with boost)
- `MONITOR`: auto|primary|<1-based index>|<part of a monitor name> (`gfx_window_win.c`, `gfx_window_sdl.c` chosen_monitor)
- `STANDIN_FILTER`: `vk_tonemap.c` vk_tonemap_record: how the scene is scaled to the picture
- `SCENE_SHARE`: `widescreen.c` plain_share: the scene's share of the picture without DLSS or FSR; 1 is the Upscaling row's Native, 0 the game's dynamic resolution. Live within the render-target heap the start made for it (heap_share).
- `DLSS_PRESET`, `DLSS_IDLE_FREE`, `TAA`: `vk_dlaa.c` fsr3
- `UPSCALER_IDLE_WAIT`: `vk_dlaa.c` vk_upscaler_idle_wait: 1 waits for the device to be idle wherever an upscaler lets go of something, as up to 0.10.1 (what was let go of still goes once the GPU is past it, behind its present gate)
- `DLSS_RANGE`: `vk_dlaa.c` range_on: one DLSS feature for the render sizes of a mode
- `DLSS_PREBLOOM`: `vk_dlaa.c` prebloom_run: DLSS or FSR 3.1 before the bloom, not after the tonemap
- `FSR4`: `vk_ffx.cpp` vk_ffx_fsr4
- `FSR4_FG`: `vk_fg.cpp` use4
- `FRAMEGEN`: `vk_fg.cpp`, `vk_dlssg.c`
- `FRAMEGEN_MULTIPLIER`: `vk_dlssg.c`: frames on screen per rendered frame with DLSS
- `FG_MULT`: the same where DLSS's frame generation runs through NGX itself (`gfx_present.c` ng_multiplier): 2x to 6x or dynamic
- `REFLEX_LIMIT`: `vk_dlssg.c` limit_allowed: with DLSS Frame Generation, Reflex holds the frame limit where each frame starts; 0 leaves it to the game's own limiter
- `SKIN_MOTION`: `vk_skin_motion.inc` skin_enabled: motion vectors of their own for every skinned character, not the player alone
- `AA`: `vk_aa.c`: what runs in place of the game's SMAA
- `GAME_ASPECT`: `widescreen.c`
- `GAME_RES`: `widescreen.c`: the picture's height
- `WIDE_HUD`: `romfs.c` wide_hud_overlay, `wide_hud.c`: the HUD's corner elements at the edges of a picture wider than 16:9 (edges) or in the corners of its middle 16:9 (center, the game's layout); the layouts are read once at start
- `WIDE_MENUS`: the pause, system, map and loading screens on a picture wider than 16:9: full fills the screen (`wide_hud.c` lays their layouts out over the whole picture), center keeps them in the middle 16:9 with black bars at the sides while one is up (`wide_bars.c` wide_menu_sides; `wide_hud.c` moves the loading icon into the middle)
- `ANISO`: `vk_texture.c` sampler_features, vk_sampler_aniso_live; 16: the game asks 1x or 2x
- `TEXTURE_DETAIL`: `vk_texture.c` "texture detail", read at the first texture
- `COLOR_PRESET`, `COLOR_BRIGHTNESS`: colour adjustment (`vk_color.c`); the preset is the menu's, the four values are what the present uses
- `HDR`, `HDR_LIVE`: HDR output (`vk_hdr.c`): on, off, the tone mapper and its grading apply at once (the HDR tonemap is spliced into the game's as shaders load whatever is set, vk_hdr_armed); native is the game's own and is asked for once, at start (vk_hdr_needs_restart). HDR_LIVE = 0 leaves the HDR path out of a run that starts with HDR off. The grading sliders: 50 leaves the picture as it is, exposure in percent. HDR_GAMMA is the display the present shows the picture as, with every tone mapper; off shows the sRGB code as it decodes, the dark tones lighter.
- `HDR_MIDGRAY`, `HDR_MATCH_SLOPE`: the observer-response mapper's: where scene grey lands, its slope there matched to the game's curve, that slope (50 unchanged) and how much of the game's hue bend at its clamp bright colours get
- `SHADOW_SCALE`, `SHADOW_COMPARE`: `vk_texture.c`
- `SHADOW_BULK_WRITE`: `vk_memory.c` shadow_write_back
- `SHADOW_TABLE_STORES`, `SHADOW_CHECK`: `vk_memory.c` "Pages a shader stores into through the device address table"
- `SVC_SLOW_MS`: `kernel.c` kern_svc: log services slower than this
- `SHADOW_PCF`: `vk_texture.c` shadow_pcf: every depth-compare sampler filters linearly (2x2 percentage-closer filtering)
- *Game*
- `RTSS`: `rtss_limit.c`: RivaTuner Statistics Server takes the limit where it runs
- `WATER_STEPS`: the set owater of `patches-v111.txt` (`frame_rate.c` frame_rate_sim_host): water and floor surfaces' fixed steps
- `SOUND_STEPS`: the set osound of `patches-v111.txt` (`frame_rate.c` frame_rate_sound_gate_host): the sound job at 60 Hz above 60 fps
- `SONG_STAFF`: the set ostaff of `patches-v111.txt`: a song the game plays back moves its staff one note for each note of the melody
- `FLOOR_CARRY`: the set ofollow of `patches-v111.txt` (`frame_rate.c` floor_carry_store): a moving floor carries a character above 100 fps as at 60
- `OCARINA_TIMING`: the set orecord of `patches-v111.txt`: the ocarina recorder's clock keeps each frame's fraction above 60 fps
- `FLOOR_PROBE`: `frame_rate.c` floor_probe: lines about Link on a moving floor (0 off, 1 = 4000 lines, or a count)
- `FLOOR_SCAN`: `frame_rate.c` floor_scan: finds the moving floor under Link in guest memory (0 off, 1 = 20000 lines, or a count)
- `FLOOR_WATCH`: `floor_watch.c`: hardware watchpoints on the floor the scan finds, at 60 fps and at 120 or more (0 off, 1 on)
- `LOG_SHORT_PATHS`: `log_privacy.c`: the account's folders and name written short in the log, the bug capture and the crash report
- `TEXT_SPEED`: `text_speed.c`: the message window's typing
- `ASSUME_FOCUS`: read on every focus change (`ov_system.c`)
- *Sound, Controls, Menu*
- `VOLUME`, `MENU_SOUND_VOLUME`, `AUDIO_CHANNELS`: `audio_mixer.c` audio_channels_live
- `AUDIO_DEVICE`: default|<1-based index>|<part of a device name> (`audio_mixer.c` resolve_audio_device)
- `MICROPHONE`: `ov_audio_input.c` mic_on: 0 refuses the game's input streams
- `AUDIO_RAMP`: `audio_mixer.c` accumulate_ramp: a changed volume is reached across the frame
- `AUDIO_CATCHUP`: `audren.c` render_thread: frames owed after a stall are rendered and dropped
- `AUDIO_STRICT`: `audren.c` audio_bad: 1 ends the process at the first game audio value the renderer cannot use, as 0.10.1 did for most of them (a limiter time that is not a number and a negative mix buffer count end it too)
- `SPRINT_BUTTON`, `SPRINT_MODE`: `sprint.c`: the sprint button (b the game's B, l3 the left stick's click) and toggle or hold; the Controls tab's Sprint rows (1.1.1)
- `DS3`: `input_ds3_win.c` ds3_read: read a DualShock 3 directly; off: no HID scan
- `RUMBLE_HD`: `input.c` hd_setting: a Switch Pro Controller or Joy-Con gets the game's vibration values as they are
- `GYRO_CALIBRATION`, `GYRO_DEADZONE`: `input.c` motion_options_refresh: the gyro's rest calibration, dead zone, and turn / tilt sensitivity and inversion
- `STICK_DEADZONE`, `STICK_SENS`: `camera_speed.c` cam_speed_tick
- `DEBUG_MENU`: `ui/menu.cpp` menu_debug_toggle
- `AMIIBO_KEYS`: `amiibo.c` amiibo_reload_keys: the 160-byte keys file; empty: key_retail.bin in the amiibo folder (AMIIBO_DIR, else ./amiibo). The Amiibo tab's Reload keys reads it again.
- `AMIIBO_WRITEBACK`: `amiibo.c` amiibo_flush_figure: also write the game's changes into the dump file (after a one-time <dump>.bak copy) when the keys decrypted it
- `AMIIBO_POPUP`: `ui/amiibo_page.cpp` amiibo_picker_wanted: the amiibo list opens over the game when it starts waiting for an amiibo
- `AMIIBO_DAILY_LIMIT`: `amiibo_daily.c`: 0 switches on the patch set amiibo_daily, which lifts the game's once-a-day amiibo limit; live, where the game version has the set. The Game tab switches it.
- `DISCORD_RPC`: Discord Rich Presence (`discord_rpc.c`): on by default (it only talks to a Discord client running on this PC, and does nothing without one); the Game tab of the settings menu and the launcher's Settings switch it. No application ID or art key is built in; a player may configure their own.
- `TRACKER_API`: The tracker API (`tracker_api.c`): off by default; on, a map tracker on this PC reads the game's memory through 127.0.0.1:<port>, read-only. Read at start: the stack and heap block is placed for it in mem_init.
- *Advanced: scheduling*
- `REPLAY_WORKERS`: `vk_exec.c` rw_init, vk_exec_replay_workers_auto
- `TAIL_EARLY`: `vk_exec.c` "the early tail"; on by default since 0.10.0
- `REPLAY_SMALL`: `vk_exec.c` group_start
- `DYN_RES`, `DYN_RES_TARGET`, `DYN_RES_STEP`: dynamic resolution (`dynres.c`): the scene follows the GPU time toward DYN_RES_TARGET milliseconds (under 1: 90 % of the frame limit's frame time), holding each step DYN_RES_STEP 60 Hz frames of the clock (1 s at least, whatever is set); without DLSS or FSR it steps the scene share down to DYN_RES_MIN at the lowest. DYN_RES and DYN_RES_MIN have menu rows (`ui/menu.cpp` row_dyn_res)
- `RECORDER_CORE`, `RECORDER_PRIO`, `READABLE_FAST`, `REPLAY_TIME`, `REPLAY_PHASES`, `LOCK_TIME`, `POOL_CREATE_BUDGET`, `POOL_CREATE_MS`, `POOL_FINI_IDLE`, `FREEZE_BUDGET_US`, `POOL_LOAD_BACKLOG`: the recording thread: a core of its own (auto: with 12 or more logical processors) and the highest priority (`vk_exec.c` rec_claim)
- `FREEZE_QUIET_MS`, `FREEZE_LOAD_HOLD_MS`, `POOL_LOAD_MS`: `vk_memory.c` freeze_pass: the backoff's quiet time and the hold around area loads
- `SUBMIT_BATCH`: held submissions handed to the queue together (`vk_sync.c`, submit batching)
- `SUBMIT_THREAD`: `vk_device.c` mesa_submit_thread: Mesa's submit thread (RADV)
- `SUBMIT_THREAD`: `vk_device.c` mesa_submit_thread: Mesa's submit thread (RADV)
- `CACHE_DIE`, `ONE_PER_CORE`: `perf.c` cpu_placement
- *shaders and pipelines*
- `PIPE_LINK_MS`: `vk_exec_draw.c` link_budget
- `PIPE_LINK_DRAW_MS`: `vk_exec_draw.c` link_draw_ms
- `PIPE_FRAME_WAIT_MS`: `vk_exec_draw.c` frame_wait
- `PIPE_SKIP_STEADY`: `vk_exec.c` steady_on
- `DRAW_WRITERS_WAIT`: `vk_exec_draw.c` writers_wait_on
- `PREPARE_PIPELINES`: `vk_pipeline.c` seed_pipelines
- `MESH_FALLBACK`, `MESH_FALLBACK_MB`: `vk_device.c`: task and mesh programs through compute passes (`vk_exec_draw.c` "mesh fallback"); auto: on a device without mesh shaders, on AMD's Windows driver for RDNA 1 and 2, on Intel's, and on RADV in the Mesa releases whose task shaders lose the device (`vk_mesh_path.h`). The megabytes are the buffers the passes write (`vk_mesh_fb.c`).
- `MESH_FALLBACK_WORKERS`: `vk_exec_draw.c` mfb_worker_count
- `MESH_FALLBACK_HOLD`: `vk_exec_draw.c` mfb_hold_role: a list's depth-only draw waits for its colour draw's pipeline
- `MESH_FALLBACK_BATCH`: `vk_exec_draw.c` mfb_batch_on: a pass's fallback draws run their stages before the pass (`mfb_batch.h`)
- `PIPE_AHEAD`: `vk_exec.c` vk_exec_ahead, `vk_pipeline.c` vk_pipeline_hint
- `LINE_WIDTH`: `vk_pipeline.c` key_lines
- `POOL_SHOWN_FIRST`: `vk_texture.c` shown_take
- `POOL_DATA_FIRST`: `vk_texture.c` data_take
- `TEX_REUSE_NAMED`: `vk_texture.c` slot_names
- `TEX_OVERWRITTEN_USED`: `vk_texture.c` drop_overwritten
- `GPL_FEEDBACK`: `vk_pipeline.c` gpl_feedback
- `GPL_MESH`: `vk_pipeline.c` mesh_lib_init
- `PIPE_FAST_WORKERS`: `vk_pipeline.c` lib_worker, fast_take
- `FRAME_HOLD_MS`: `vk_backend.c` frame_hold
- `PACE_SMOOTH`: `vk_backend.c` pace_smooth
- `OQ_BATCH`: `vk_exec_xfer.c` oq_take
- `CLEAR_SMALL_CPU`: `vk_exec_xfer.c` clear_small
- `OQ_HOLD`: `vk_exec_xfer.c` oq_report_count: a samples-passed report missing draws the full query ring left uncounted repeats its record's last complete count where that is more than the part counted
- `QUAD_INDEX`: `vk_exec_draw.c` draw_idx_rewritten: indexed quads drawn as two triangles each, indexed line loops closed
- `PATCH_OCCLUSION_OFF`: the patch set occlusion_off (`recomp/patches-v111.txt`): the game makes no occlusion queries. The row is what gives the set its default (`patches.c` rt_patches_init).
- `GATE_WAIT_MS`: `vk_exec.c` vk_exec_gate_wait
- `LIB_WARM`: `vk_pipeline.c` lw_start: the driver's cache of stage libraries, warmed when cold
- `PREPARE_STALL_S`: `vk_pipeline.c` prep_stalled: seconds the shader preparation waits for builds the driver still holds once the rest are done; the game goes on with them in the background. 0 waits for every build
- `PREPARE_STALL_TEST_MS`: tests of prep_stalled: the last variant waits this long first
- `PIPE_PREBUILD`, `PIPE_SEED`, `PIPE_SEED_BOOT`: `vk_pipeline.c` keys_open_seed
- `PREWARM_XLATE`, `GPL`: `vk_caps.c` vk_caps_gpl: by the driver, off with AMD's for RDNA 2 and older
- `PIPELINE_CACHE_MAX_MB`: `vk_pipeline.c` cache_cap: unset, from the RAM (1024 to 4096)
- `FFX_PIPE_CACHE`: `vk_ffx.cpp` vk_ffx_proc_addr: FSR 3.1's pipelines through the pipeline caches
- `PIPELINE_CACHE_PLAY`, `PIPELINE_CACHE_PLAY_MB`: `vk_pipeline.c` "the play cache": what a run compiles goes into a file of its own
- `PIPELINE_CACHE_STAMP`: `vk_pipeline.c` cache_stamp_check: files of a translator nothing serves any more are deleted
- `NVIDIA_CACHE_DIR`: `vk_device.c` nvidia_cache_beside_exe: NVIDIA's shader disk cache in nvidia-cache beside the exe
- `BUILD_FREE_MB`, `BUILD_RESERVE_MB`: `vk_pipeline.c` gate_enter
- `BG_BUILD_PLAY_DRAWS`: `vk_pipeline.c` play_rest: background builds rest in heavy frames
- `MEM_NEED_MB`: `memory.c` mem_start_note; the default is `mem_need.h` MEM_NEED_DEFAULT_MB
- `COMPUTE_KEEP`: all|<count>: the compute pipelines the boot keeps, those dispatched most recently (`vk_pipeline.c`, compute-used.bin)
- `SHADER_CARRY`: `gfx_shader.c` "Carry": a previous translator's translations until translated again
- `SHADER_COMPRESS`: `gfx_shader.c` "SPIR-V at rest": a shader's SPIR-V is kept zstd-compressed in memory once no pipeline was made of it for the idle seconds; 0 keeps every shader's SPIR-V plain
- `TEX_RELEASED_MAX`: the settings the low-end GPU mode had rows for up to 0.10.1 (lowend_set, below): each is read by itself now `vk_texture.c` released_max: "" by the video memory budget, else 1..16384
- `SUBMIT_FINISH_KINDS`: `vk_sync.c` res_mask
- `FOLIAGE`: `vk_exec_draw.c` foliage_skip
- `VRAM_HEAP_LIMIT`: `vk_device.c` vram_heap_limits: auto, 0 or MiB
- `VRAM_RESERVE_MB`: `vk_vma_budget.c`: video memory kept free of VMA's allocations
- `UPLOAD_BATCH_MB`: `vk_memory.c` vk_upload_end: MiB of staging before a batch goes early. 0: at 64 MiB, the staging pool's own limit (STG_FLUSH), which also holds for every value above it
- `GUEST_OPT`: `tools/make_game.py`: the next Rebuild the Game
- `PASS_TIMES`, `DUMP_KEY`: `vk_crumbs.c`, `vk_dump.c`
- *textures and memory*
- `TEX_RELEASED_HOLD`, `ASTC_BC`, `ZSTD_NATIVE`, `ZSTD_CHECK`: `vk_texture.c` drop_aged: seconds a released texture is kept unused (`vk_tex_release.h`)
- `LOW_END_GPU`: the switch of the low-end GPU mode, which is gone: still a known key, and it does nothing (lowend_set, below)
- `ASTC_SCRATCH_KEEP`, `ASTC_ARENA`: `vk_astc.c` scratch_get, `vk_texture.c` astc_arena
- *render fixes*
- `WATER_PROJECTIVE`: `vk_pipeline.c` water_projective_pair
- *Vulkan*
- `VK_RECONVERGE`: `vk_pipeline.c` spirv_reconverge
- `VK_MESH_SUBGROUPS`: `vk_pipeline.c` mesh_stages_init, stage_sg
- `VK_MESH_INVARIANT`: `vk_pipeline.c` spirv_mesh_invariant
- `VK_NAN_PRESERVE`: `vk_pipeline.c` spirv_nan_preserve; unset: every driver but NVIDIA's
- `VK_NAN_BITS`: `vk_pipeline.c` spirv_nan_bits; unset: AMD's driver for RDNA 1 and 2
- `VK_NOOPT`: `vk_pipeline.c` noopt_flag_for; unset: mesh on AMD's driver for RDNA 1 and 2
- *diagnostics*
- `STUTTER_MAP`: `stutter_map.c`: hitches by area and cause, and the pipelines and shaders draws met missing (0 off, 1 on)
- `PAR_JOBS`: `par_jobs.c`: the worker pool that splits a large texture's copy out of the game's memory (0: the thread that makes it alone)
- `PIPE_RELEASE`: `vk_pipeline.c` "pipelines no draw has used, given back while video memory is short" (0: every pipeline stays)
- `FLIGHT_FLASH`, `FLIGHT_FLASH_DIFF`, `FLIGHT_FLASH_CELLS`: `flight.c` "The flash trigger": on or off, and `flight_flash.h` flash_test's two thresholds
- `MIRROR_CAPTURE_KB`, `WB_SKIP_MB`: `vk_memory.c` capture_cap, wb_skip
- `WB_MERGE_MAX`, `TEXUP_LOG_MS`: `vk_texture.c` vk_texstat_frame
- `WAIT_WATCH_S`: `kernel.c` wait_watch: a lock or thread-end wait this long is logged; 0 off
- `PRESENT_ORDER`: `vk_backend.c` present_order_on: a frame's bookkeeping between two replays (`vk_frame_owed.h`); 0 at the present
- `HANDLE_CENSUS`: `perf.c` hc_main: with NX2SARIA_TRACE=perf, the process's handles by kind when their count grew
- `HANG_WATCH_S`: `hang_watch.c`: a report when no frame is shown with the window in front, or the GPU does not finish, this long; 0 off
- `VMA_BLOCK_MB`: `vk_headroom.c` vk_vma_block_size
- `TEX_COMPACT`, `TEX_COMPACT_MIN_MB`: `vk_texture.c` compact_on: idle textures move out of sparsely used blocks of video memory; the free space inside the blocks from which a pass starts, and the bytes moved per frame (also the largest texture that moves)
- `ASTC_BC1`: `vk_astc.c` bc1_on
- `VRAM_PAGEABLE`: `vk_device.c`: pageable video memory with priorities
- `TEX_RESIDENCY`: `vk_texture.c` residency_step
- `POOL_TEX_RELEASE`: `vk_memory.c` vk_mem_tex_pools
- `PASS_CRUMBS`: `vk_crumbs.c` pass marks; unset: VK_AMD_buffer_marker only
- `DRAW_CRUMBS`: `vk_crumbs.c` draw marks: a mark at each change of shaders inside a pass
- `STAGING_FAIL_AT`: `vk_memory.c` staging_fail_now: a test of give_back
- `VRAM_BALLAST_MB`: `vk_memory.c` vk_mem_ballast: a test of a smaller card
- `GEN_SEED`: `vk_memory.c` gen_seed: a test that starts the page write generations at this value (at most 2^62)
- `VRAM_WARNING`: `vram_warn.c` vram_warn_start: 0: no notice on screen when the video memory stays critical; the `vram_warn:` log lines of a notice then do not come either
- `MEM_BUDGET_TEST_VRAM_MB`: `mem_budget.h`: a test that pretends the video memory or the system memory is that many MiB, for the levels the owners that give memory back go by; 0 as it is
- `DEVICE_LOST_AT`: `vk_sync.c` lost_at_due: a test of the lost device's end
- `COPY_RETIRE`: `nvn_api_cmd.c` retire_pass: counts the recording copies whose control memory the game reused; frees nothing

### `is_known()`

- `return !strncmp(key, "CHEAT_", 6);`: `cheats.c` and the cheat patch sets (`tools/cheats.py`)

### `unknown_keys`

The keys of the file's lines that are not settings of this version (add), lower-case, ", " between them, each once; what did not fit is counted and left out. Written by cfg_load, before any other thread reads settings.

- `static int n_unknown, unknown_cut;`: how many; whether the list left some out

### `note_unknown()`

A key of other characters (a line of text with an '=' in it) is counted, not listed: the menu cuts the list by bytes to fit its row

- `for (const char *p = unknown_keys; (p = strstr(p, low)) != NULL; p ...`: a repeated line counts once

### `cfg_unknown_keys()`

The table holds CFG_MAX_KEYS keys and keeps none past them: a file's later keys were not read, and a menu change was lost, without a word (every cheat ever switched keeps a line). A line says so once per load, and once a run for the changes (cfg_set, cfg_reset, migrate).

### `norm_key()`

The upper-case key without the prefix, in place.

### `entry_line`

- `static int entry_line[CFG_MAX_KEYS];`: the file line each entry's value came from (cfg_load's check_values)

### `CHECK_LINES`

A comment after a value (cfg_load) "pace_smooth = 0 ; off" kept the comment in the value, which cfg_bool read as on, and "fps_limit = 144 # monitor" gave 60. For a key whose row has a set of values (numbers, ranges, words: none has a blank followed by ';' or '#') the value ends at the first blank followed by ';' or '#' in the text after '=' ("key = ; note" is empty), and the line is logged once at load. Free text keeps the whole value: a row without a set (window_size, monitor, audio_device) and a key without a row (mods_dir, pipeline_cache, the bindings), where a path or a name may hold " #". So does a value in double quotes. The menu's save writes a changed key's line again without its comment (cfg_save_to), the same way every time. At most CHECK_LINES lines per load, then a count (check_end).

### `cut_comment()`

Raw: the text after '=' as written; returns the value trimmed, cut at its comment when key has a set of values.

### `OLDER_MAX`

Value: the text after '=' as written (cut_comment trims it).

Older names: a setting set as NX2NAVI_<KEY> in the environment or written so in the settings file is read as NX2SARIA_<KEY> (cfg_env, norm_key). The log says so once per name and place in a run, the first time it is seen: the environment's at cfg_init (older_env), the file's at a load or a reload of it (older_file_key). A name the runtime does not read (is_known) gets no line. At most OLDER_MAX lines, then one saying the rest are not listed.

### `older_told`

- `static char older_told[OLDER_MAX][72];`: "e:" or "f:" (environment, file) and the upper-case key without a prefix

### `older_first()`

1 the first time where:key comes by (where 'e' or 'f'); 0 after, and once OLDER_MAX are told.

### `older_file_key()`

Raw: a key as the settings file at line writes it, before norm_key.

The environment's NX2NAVI_<KEY> variables: read where NX2SARIA_<KEY> is not set (cfg_env), and not where it is.

### `CFG_ENVIRON`

- `#define CFG_ENVIRON _environ`: the C runtime's copy, which getenv reads (main, not wmain: set up at start)

### `older_env()`

The name as cfg_env asks for it, in upper case: on Linux a variable in another case is not read, and gets no line

### `add()`

- `if (!*value) return;`: "key =" leaves the setting at its default
- `if (!strcmp(entries[i].key, key)) {`: a repeated key: the later line wins

### `misfits`

Values outside their rows' sets, logged at load (the values are read as they are written, a comment after a value of a row with a set cut first, cut_comment) Hand-edited files had values no consumer takes, and nothing said so: "sharpness = 250" is past 0..100. Only the sets that are the consumers' own are checked: 0|1 (with the words switch_word reads), ranges lo..hi, and lists of words. A list with a number in it is the menu's presets, and its consumer takes more (render_scale 25..400), so it is left alone. A list may hold one range among its words (fps_limit 30..240|display|unlimited): a number fits it as it fits a range alone. A list of words names the menu's choices; a consumer may take more (dlss_preset any letter a..m), so the line says no more than that. A ';' or '#' after a blank is still named where it stays in the value: a free-text row's value, and the environment's. At most CHECK_LINES lines per load, then a count.

### `range_fit()`

Whether v is a number in the range m[0..n) ("lo..hi"): 1 yes, 0 no, -1 when m is not such a range.

- `*dots = 0;`: the low end apart: strtod would take "0." of "0..100" for a number

Any number: some rows with whole ends take fractions (dyn_res_target 16.7 in 0..100)

### `misfit()`

NULL when v fits the set, else what the log line says of it (in buf).

- `if (fit) return NULL;`: fits, or a set that is no range this reads
- `for (const char *m = set; *m;) {`: a member that is a number: the menu's presets, not checked

### `check_value()`

Key: upper case without the prefix; line 0: the value comes from the environment.

### `migrate()`

0.1.9 had the Xbox prompts as a pad layout: pad_layout = xbox_prompts is pad_layout = nintendo with prompts = xbox. Read as those two, which the menu shows and the next cfg_save writes; a prompts line of the file's own wins. The value still works from the environment (input_pad_layout_pick, input_prompts_pick).

### `utf16_text()`

A file saved as UTF-16 (Notepad's "Unicode": the byte order mark FF FE or FE FF, then two bytes per character) as UTF-8 in a new buffer; NULL for any other file. Read as bytes such a file ends at the zero after its first character: no line was taken, and every setting kept its default without a word in the log.

- `if (c >= 0xD800 && c < 0xDC00 && i + 3 < size) {`: a pair: one character above U+FFFF

### `parse_ini()`

Calls sink(key, value, line, ctx) for each "key = value" line of the file at path (key and value trimmed, in place). 0 when the file is missing.

- `if (size >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;`: editors add a BOM
- `sink(trim(s), eq + 1, line, ctx);`: the value as written: the sink cuts a comment (cut_comment)

### `cfg_load()`

- `__atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);`: the lookup memo (cfg_memo) forgets the freed strings
- `check_values();`: after migrate: a value it rewrites is checked as the game reads it

### The table `DECK[]`

Device profiles: defaults for a known machine, below the environment and the settings file and above the quality preset
and the table, so any key the player sets wins and resetting a key in the menu returns to the profile's value.
`NX2SARIA_DEVICE_PROFILE` is `auto` (the default: the Steam Deck when the firmware says so), `desktop` (none) or `deck`.

The Steam Deck (LCD Jupiter, OLED Galileo; 1280x800, 16:10, 4 Zen 2 cores, RDNA 2 with 8 CUs sharing 16 GB): the game
uses the monitor's 16:10 shape (Ocarina chooses its picture with `GAME_ASPECT`/`GAME_RES`, not `OUTPUT_RES`);
fullscreen and vsync, as gamescope composes Game Mode; 40 fps, which divides 40 Hz (and 120 Hz on the OLED); the game's
six virtual cores folded onto four, one per physical core (`kernel.c` cores_enabled); ASTC textures stored as BC3, as
the automatic choice gives with 16 GB shared between the CPU and the GPU; and a spinning guest thread handed off after
30 us instead of 50 (`kernel.c`), with four cores to share. These are starting points to replace with measured ones
(docs/tester-guide.md, Steam Deck).

`CFG_TEST_DECK_ROW` is for `tests/preset_check.py` only: a row of its own, for a key a tier also sets with another value.

### `profile`

- `static int profile = -1;`: 0 none, 1 Steam Deck

### `on_steam_deck()`

The Steam Deck: Valve's board, product Jupiter or Galileo (/sys/class/dmi/id); SteamDeck=1, which Steam sets on it.

### `lowend_set()`

Low_end_gpu (NX2SARIA_LOW_END_GPU, formerly NX2NAVI_LOW_END_GPU) was the switch of the low-end GPU mode up to 0.10.1. With it on, nine keys got other defaults (mesh shaders run as compute where the card has none, a texture read after another image drew over its pool memory given memory of its own, game memory copied for the GPU while the replay records, video memory kept within the heaps' budgets, large upload batches sent early, pools the game refills every frame kept in system memory), and those keys and a dozen more were read only while it was on. The mode is gone. Its switch is still a known key, so that an old file's line is no unknown one, and does nothing. Each of its keys is read like any other, with the default of its row or of its reader: a value a player set by hand counts, and a file that sets none runs the game as it runs for everyone else. cfg_init says so once while the switch is on. Read through raw_get, which skips switch_word, so the words are taken here as cfg_bool takes them: "low_end_gpu = off" is off, and any value that is not 0 or one of the off words is on.

### `preset_hw`

Quality presets: defaults below the device profile and above the table (`preset_pick.h`). cfg_start (preset_init) reads the hardware into preset_hw once and then turns the layer on; the menu can switch it to another tier (cfg_preset_use). preset_hw does not change after the layer is on, and the tier is one atomic int, so readers take no lock; every value is a literal of PRESET_ROWS, valid for the process.

### `preset_tier`

- `static int preset_tier = -1;`: the tier whose rows make the layer; -1: no layer (before cfg_start)

### `cfg_env()`

NX2SARIA_* is the current public name; NX2NAVI_* remains an alias for existing scripts. When both exist, the current name wins even when it is empty, so an explicit new setting can clear a legacy override.

### `switch_of()`

A switch written as a word: 0 for false, off and no, 1 for true, on and yes (any case); -1 for anything else but 0 and 1, which give themselves.

### `switch_word()`

The value of a switch (a row whose set is 0|1) written as a word, as "0" or "1" for every reader: cfg_bool, rt_knob and the readers that compare with "0" (`frame_rate.c` frame_step_cap_ns for rtss) or parse a number (cfg_int). "fullscreen = false" and "rtss = off" were on, read as anything but "0". Other values and other rows are handed on as they are.

### `cfg_is_set()`

Whether name's value comes from the player (the environment or `nx2navi.ini`), not a profile or the table.

### `set_value()`

Cfg_set and cfg_set_test: one change under one hold of set_lock, so a save on another thread (the one at exit) never sees a test value before it is marked as the test's. test 0: the value is the player's, and saved; test 1: the test's (cfg_set_test), unless the entry already holds that value as the player's unsaved change, which stays theirs.

- `if (entries[i].value) kept_n++, kept_bytes += strlen(entries[i].val...`: cfg_sizes
- `__atomic_store_n(&entries[i].value, strdup(value), __ATOMIC_RELEASE);`: the old string stays for readers
- `if (test && !changed && entries[i].dirty && !entries[i].test) test ...`: the player's unsaved change
- `entries[i].removed = 0;`: a value set after cfg_reset replaces the line instead of deleting it

### `cfg_reset()`

- `if (cfg_env(var)) continue;`: the environment wins; the file line stays
- `if (i == n_entries) {`: no value in memory ("key =" in the file): the line still has to go
- `__atomic_store_n(&entries[i].value, NULL, __ATOMIC_RELEASE);`: the old string stays for readers
- `entries[i].removed = 1;`: cfg_save_to deletes the key's lines
- `entries[i].test = 0;`: the removal is the player's, and saved

### `find_entry()`

The entry of upper-case key without the prefix, or -1. set_lock held.

### `cfg_hold()`

- `if (i < 0 && on && n_entries < CFG_MAX_KEYS) {`: a key not set yet: an empty entry the following cfg_set fills

### `cfg_revert()`

- `__atomic_store_n(&entries[i].value, (char *)orig_value, __ATOMIC_RE...`: the strings are never freed
- `entries[i].test = (orig_state & CFG_TEST) != 0;`: a test value put back stays the test's

### `CFG_MEMO_N`

Lookup memo. cfg_get reads the environment (getenv walks the whole block comparing names) and then the file's entries and the settings table by name, and several per-draw paths ask for live settings: on the replay thread those lookups were about a sixth of the samples in a play profile (ucrtbase's string routines). Each thread keeps the answers it got per name until the settings version changes (cfg_set, cfg_reset, cfg_load); values stay valid that long because a set keeps the old string for readers and a load bumps the version. NX2SARIA_CFG_MEMO=0 (environment only, read once) asks every time.

### `cfg_memo_get()`

- `unsigned ver = __atomic_load_n(&version, __ATOMIC_ACQUIRE) + 1;`: 0 marks an empty slot
- `const char *v = kind ? cfg_value_now(name) : cfg_get_now(name);`: read after ver: a change meanwhile misses next time

### `default_number()`

The row's default as a number, for a set value that does not parse; 0 for a key without a row, whose set value cfg_value hands out without one (`tests/cfg_rows_check.py` keeps such reads guarded).

### `cfg_bool()`

Off for "0" and for false, off and no in any case (switch_of), also for a key whose row is not 0|1; on otherwise.

### `at_start`

The restart-only settings' values at start (cfg_start), which the running game keeps using whatever the menu sets later: cfg_pending compares against them, so the menu says "after restart" only while a restart would change something. One per row of metas, NULL for a live one.

### `start_values()`

- `if (!metas[i].live) at_start[i] = strdup(meta_value(i));`: a copy: cfg_load (the probes) frees what it read

### `pending_at()`

A value the environment sets never waits: the menu cannot change it and a restart keeps the environment. The command line's --monitor and --audio-device arrive that way after the start values are taken (`main.c`).

### `cfg_pending_for()`

As cfg_pending, for a value the key may get: the settings menu asks it of the value its draft holds, which waits for Apply and so is not cfg_value yet.

### `line_count()`

Cfg_save: the file as lines, each with its line ending

### `line_text()`

The line's text without its ending, BOM and surrounding blanks, into out.

### `line_key()`

The key of a "key = value" line (comment = 0) or of a commented "# key = value" line (comment = 1), normalised, into key; 0 when the line is not one.

### `line_section()`

The section name of a "[name]" line, lower-case, into out; 0 when the line is not one.

### `section_of()`

- `if (!strncmp(key, "CHEAT_", 6)) return "cheats";`: `cheats.c`: one key per cheat of the cheat file

### `cfg_has_section()`

1 when key has a section of its own (the probe checks every row of the settings table has one).

### `section_for()`

The section cfg_save_to writes key under. A key with none goes to [misc]: a bare line appended at the end of the file would belong to whatever section happens to come last (the loader ignores section names, but a person does not).

### `put_value()`

A value as cfg_load reads it back: quoted when it starts or ends with a blank (cfg_load trims those) or is itself quoted (cfg_load takes one pair of quotes off). A mod's folder name in mods_order can start with a space.

### `put_entry()`

"key = value" + eol for entry e (key lower-case)

### `park_held()`

A held entry (cfg_hold: a value the menu previews and the player has not applied) is invisible to a save: its flags are parked here for the length of cfg_save_to and put back after it, so a save made by anything else (F10, F11, closing the window) never writes it. So is a test hook's value (cfg_set_test): a test run's settings stay out of the file, the save at exit included, and the log says how many were left out, again only when that number changes.

### `cfg_save_to()`

The file is there but unreadable (locked, transient error): rebuilding it from nothing would lose its lines

After[i]: entries to write after line i (-1 = none); replace[i]: the entry whose value line i now carries

- `char *drop = calloc((size_t)nl + 1, 1);`: lines of a reset key
- `if (nl && !lines[nl - 1].n) nl--;`: the text ended with a newline: no last empty line
- `after_key[e] = -2;`: -2 clean, -1 append in a new section at the end, >= 0 insert after that line

The line leaves the file; keys inserted after it still need a line ending before them

Keep the line's indentation and key spelling, put the new value after " = "

Keys whose section the file lacks: one new section each, at the end

The following keys of the same new section go right after this one

- `char names[256];`: which keys this save wrote, so a log shows what reached the file and when

### `find_ini()`

The settings file beside the game: `nx2saria.ini`. An `nx2navi.ini` there without one, the name before NX2Saria (any version of NX2navi), is renamed to it the first time the game looks, so a player who moves over keeps every setting and the folder holds one settings file from then on; `launcher/gameini.py` does the same where the launcher reads it first. The rename never replaces a file (plat_fs_rename); where it fails, the old file is read and written under its own name. With both there, `nx2saria.ini` is the one, and the log says the other is not read.

- `if (!cut || cut + 13 >= path + sizeof path) return 0;`: room for "`nx2saria.ini`" and its NUL after the separator

### `cfg_save()`

A replay of a bug capture (`bugcap.c`) runs on the capture's settings and leaves the player's settings file as it is: nothing is saved, whatever the run sets.

### `log_file_settings()`

What the file sets, in the log once at start: the settings of this version with their values, in lines of up to about 900 characters, and then the keys that are no settings of this version, which do nothing. A report's log then shows which settings its run had and which of the file's lines had no effect; before, it held only the count of keys, and a player who had added a line this version does not read could take it for the cause of what changed.

A private setting (`cfg_private.h`): only that it is set

- `if (vl > 200) vl = 200;`: a long value (a list of mods) is cut: the line shows that the key is set

### `file_sink()`

As add reads a line: the comment after a value cut where cfg_load cuts it (cut_comment), one pair of quotes off.

- `if (!*key || !*value) return;`: "key =" leaves the setting at its default

### `file_migrate()`

Migrate() on the file's list: pad_layout = xbox_prompts reads as pad_layout = nintendo and prompts = xbox.

### `whole_in()`

Validation: a reloaded value is checked before it reaches a consumer. At start a bad value is the player's to fix (some consumers stop the game with it: ov_language, ov_handheld, mem_init); a reload must never do that, so a value that does not fit is rejected and the current one stays. The check is the one cfg_load logs at start (misfit: 0|1 with the switch words, a range lo..hi, a list of words; a list with a number in it is the menu's presets and is not checked); keys whose consumers take more than their row lists, or stop the game on a miss, have their own rule in value_ok, and keys without a row or a set (bindings, mod lists, cheats, free text) have consumers that fall back on their own and are not checked.

### `size_in()`

- `static int size_in(const char *v, long min_w, long min_h, long max_...`: "WxH"

### `listed_exact()`

V is one of the row's "a|b|c" values, case and all (ov_language and ov_handheld compare exactly).

### `value_ok()`

- `if (!strcmp(key, "APP_POOL")) {`: mem_init stops the game on 0

### `env_sets()`

- `static int env_sets(const char *key) {`: the file's new value would not show

### `reload_one()`

One key of the file that differs from memory: entry e, or NULL for a key the file gained (want is then set). set_lock held. Counts the outcome in r.

- `if (e && (e->held || e->dirty || e->removed)) {`: a menu change not saved yet: the file would overwrite it
- `if (e->value) kept_n++, kept_bytes += strlen(e->value) + 1;`: cfg_sizes
- `__atomic_store_n(&e->value, want ? strdup(want) : NULL, __ATOMIC_RE...`: the old string stays for readers
- `if (!cfg_supported(key)) {`: stored, so memory and file agree, but nothing reads it on this build

### `cfg_reload_file()`

- `if (bugcap_replay_dir()) {`: the capture's `settings.ini` stands in for the player's settings file (load_ini)
- `check_end();`: the comments cut_comment named, counted afresh for this read
- `int preset_before = preset_parse(raw_get(PREFIX "PRESET"));`: preset_follow: whether the reload changes it
- `for (int i = 0; i < f->n; i++) {`: keys the file gained

### `PRESET_KEYS_MAX`

Quality presets: the tier at start, the state file, and what the settings menu asks and does

Cfg_start (preset_init), after the settings file is read and the guest address space is claimed (`memory.c` mem_init) and before the restart-only values are kept: reads the hardware (`hw_detect.c` hw_detect), asks `preset_pick.h` for the tier this PC should get and turns the layer on with the tier in use. preset = recommended (the default) is that tier, decided anew at every start, so a first start and a start on new hardware get theirs. A tier the player chose is used as it is, unless it was chosen on other hardware (the state file's chosen_on against this start's fingerprint, the driver version left out: `preset_pick.h` preset_same_hardware): this run then uses the recommended tier, the log says why and cfg_preset_hw_changed tells the menu, until the player picks a tier again (cfg_preset_chosen). A tier without a chosen_on is taken as chosen on this PC, and this PC's fingerprint is recorded. A run that replays a bug capture uses high, which sets nothing: the capture's `settings.ini` holds the values its own preset gave (`bugcap.c`), and the replaying PC's preset would change them.

The state file is `preset-state.txt` beside the game executable and `nx2saria.ini`: the game's own record of what it saw, never settings; nothing here writes `nx2saria.ini`. Lines "key = value": hardware this start's fingerprint (`preset_pick.h` preset_fingerprint) chosen_on the fingerprint when the player last chose a tier; absent while none is recorded noticed the recommended tier the one-time notice last named (cfg_preset_notice_due); absent before any gpu_* the last probe that found a GPU (name, vendor, device, driver, type, vram_mib, mesh): a start that cannot probe (`hw_detect.c`) goes by them; a GPU missing any of them is none probing 1 from right before the probe's first Vulkan call until it returns (preset_probe_begin). A start that finds it skips the probe and goes by the GPU above: the probe runs before the crash report and the log file are set up (`main.c` main_run), so a driver that crashes or hangs in it would otherwise stop every start. It stays until the game has chosen its own Vulkan device (cfg_preset_device_started), which shows the drivers load, or the line is deleted by hand; every start that finds it says so in its log. It is written through a temporary file and a rename (plat_file_write_atomic), only when its text changes, and only by the game itself: not by a tool mode (cfg_start's game 0) and not in a run that replays a bug capture.

### `preset_lock`

- `static PlatMutex preset_lock = PLAT_MUTEX_INIT;`: the state file and the fields below once cfg_start has returned
- `char path[1024];`: the state file; "" when the executable's folder is unknown
- `char text[2048];`: the state file's text as read or last written
- `char logged[2048];`: the text as read or last written with a log line (state_write's quiet writes aside)
- `char chosen_on[160];`: "" while none is recorded
- `char noticed[16];`: the tier the one-time notice named, "" before any
- `char fp[160];`: this start's fingerprint; until the probe returns, the state file's hardware line
- `char why[192];`: why the recommended tier
- `PresetHw gpu;`: the state file's last probe (have_gpu 0 without one)
- `int rec;`: the recommended tier
- `int write;`: the state file may be written: the game itself, not a replay
- `int probing;`: the state file's probing mark (above): found at start, or the probe runs now
- `int hw_changed;`: atomic: the player's tier gave way to the recommended one (above)
- `int n_keys, keys_done;`: keys_done atomic

Every key some tier sets on this PC's hardware, in the table's order: the keys among which the menu hands back to a preset, and Custom counts, those the tier in question sets (cfg_preset_differs). A key only another kind of PC gets from a tier (fps_limit, set on integrated GPUs and handhelds alone) stays the player's own here.

### `preset_keys_init()`

- `if (!preset_applies(PRESET_ROWS[i].when, &preset_hw)) continue;`: no tier sets it on this PC

### `preset_parse()`

"low".."ultra" to their tier, "recommended" (or nothing) to -1, anything else to -2.

### `state_number()`

A whole number of the state file (decimal or 0x hex) into *out; 0 when v is not one.

### `state_read()`

Reads the whole file, line by line to its end: a blank line or a stray NUL byte ends only its own line. A kept GPU needs every one of its seven lines with a value of its kind, else none is kept: a GPU with fields left at 0 would give a tier of its own (no mesh shaders: Low).

- `else memset(g, 0, sizeof *g);`: a GPU without all of its lines: none kept

### `state_write()`

Preset_lock held, or cfg_start. quiet: no "wrote" line (the probing mark and its removal, which leave the file as it was when nothing else changed); a failed write is logged either way.

### `state_write_main()`

The state file written on a thread of its own, for a caller on the present thread (the one-time notice, a Ctrl+F5 that changes the preset line), which should not wait for the disk. Written inline when no thread starts.

### `preset_probe_begin()`

`hw_detect.c` hw_detect, right before the probe's first Vulkan call: one log line (before log_open it waits in `main.c`'s early buffer and is on stderr at once), and the probing mark in the state file, which the end of preset_init takes out again once the probe has returned.

### `same_value()`

Two values of one setting the same: equal apart from case, or the same number ("0.50" and "0.5").

### `preset_log_keys()`

The log's lines of each key the tier sets: where the value in effect comes from. Once per start.

### `preset_init()`

- `if (!skipped[0]) PS.probing = 0;`: the probe returned: the end's state_write takes the mark out
- `PS.gpu = hw;`: the state file keeps its GPU fields
- `} else if (skipped[0] && PS.gpu.have_gpu) {`: no probe this start: the last one's GPU
- `__atomic_store_n(&PS.keys_done, 0, __ATOMIC_RELEASE);`: the key list again, for this hardware

The Steam Deck's GPU runs at the clock its power management picks; asking the amdgpu driver for its stable peak clock through the render node would raise the frame rate there, at more power. Nothing here does that yet: it waits for someone with a Deck to measure it, and this line marks where it would go.

- `if (want == -2) {`: check_value has named the value

A tier the environment sets is used as it is: the new-hardware rule is for the settings file's line, which the player chose on some PC; the environment's is set for this run, and is no choice recorded here.

- `__atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);`: the lookup memo forgets what it read without the layer

### `preset_follow()`

Cfg_reload_file: the file's preset line changed (Ctrl+F5) from before, the tier the line named before the reload (preset_parse): the layer follows it. The line as it was, not the tier at start: the menu may have switched since. An edited line is the player's choice on this PC, as one made in the menu: a tier chosen on other hardware no longer gives way (cfg_preset_hw_changed), and a tier is recorded as chosen here (written on a thread of its own: Ctrl+F5 runs on the present thread).

### `cfg_preset_notice_due()`

- `if (set && *set) return 0;`: the player chose, recommended included: nothing to tell

### `cfg_preset_notice_done()`

- `state_write_later();`: called on the present thread (`ui/menu.cpp` preset_notice_tick)

### `cfg_preset_device_started()`

- `state_write_later();`: called where the game picks its device (`vk_device.c`); the disk is not waited for

### `cfg_preset_chosen()`

- `else PS.chosen_on[0] = 0;`: nothing to compare a later start with: recorded at the first start that knows its GPU

A player who chose a preset in the menu has seen it: the one-time notice (cfg_preset_notice_due) has nothing left to tell, also when the choice was Recommended, which leaves the settings file without a preset line

### `load_ini()`

NX2SARIA_CAPTURE_REPLAY=<a bug capture's folder>: the settings that capture was taken with (its `settings.ini`) stand in for the player's settings file, which is neither read nor written in such a run; the environment still wins over them

- `if (cfg_env(var))`: the name that is set: the current one wins when both are

### `cfg_init()`

- `for (size_t i = 0; i < sizeof metas / sizeof *metas; i++) {`: the environment's values, as the file's (cfg_load)

### `cfg_start()`

- `preset_init(game);`: before the restart-only values are kept: a preset's are among them

### `cfg_sizes()`

NX2SARIA_TRACE=perf "mem: sizes" (`perf.c`): settings entries, and the replaced values cfg_set keeps for readers.

## `runtime/cfg/cfg_private.h`

Settings whose values stay on the player's PC: the menu's test hook, and what the co-op of versions up to 0.9.2 kept in `nx2navi.ini`, which a player's file may still hold (a room's secret and the offer number made for it, the relay's address, the player's name, the key a maker removed). The log's line of what the selected settings file sets (`cfg.c` log_file_settings), "Copy system info" (`sysinfo.c`) and the copy of the settings file a bug report takes along (`save_share.c`; `launcher/saves.py` has the same list) show only that such a key is set. Header only, so that the tests that build one of those files alone need nothing else.

### `cfg_private_key_n()`

Key: a setting's name, optionally carrying either supported prefix, in any case, n bytes of it.

### `cfg_ini_scrub()`

A copy of the text of an `nx2navi.ini` (n bytes) in which every private key that has a value reads "key = (set, not shown)"; everything else, comments and line ends included, byte for byte. malloc'd, *out_n its length; NULL when memory runs out.

- `size_t stop = end;`: the line's text without its line end
- `if (at == 0 && n >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) k0 = 3;`: a byte order mark before the first key
- `memcpy(out + o, text + stop, end - stop), o += end - stop;`: a carriage return, if the line had one

## `runtime/cfg/cfg_support.c`

Whether a setting does anything on the running game build (`cfg.h` cfg_supported). The menu hides a row whose key it answers 0 for (`ui/option_rows.cpp`), and the start-up log names the keys a player has set that are inert. The answers follow the generated patch sets of recomp/patches-v*.txt (rt_patch_set is -1 for a set the build lacks) wherever a setting needs one; the few that depend on the game itself check rt_game_version.

### `cfg_supported()`

- `if (!strcasecmp(name, "FOV")) return has("fov");`: `resolution.c` res_fov_live

`resolution.c` decide reads both, and only res_init's patch of the output and scene sets uses them

- `if (!strcasecmp(name, "SHADOW_RES")) return has("shadow");`: `resolution.c` shadow_init
- `if (!strcasecmp(name, "SHADOW_RANGE")) return has("cascade");`: `resolution.c` res_shadow_range
- `if (!strcasecmp(name, "REALTIME")) return has("realtime");`: the set's switch (`patches.c`), `frame_rate.c`
- `if (!strcasecmp(name, "CLOTH_REALTIME")) return has("cloth");`: the set's switch (`patches.c`)

The switch of three sets (recomp/patches-v*.txt): bounce, fade and cursor

`menu_cursor.c` enabled() on 1.4.3, `menu_mouse.c` (its screens' site, set menu_mouse) on 1.1.1

The squeeze of the UI into the middle 16:9 (set wide_hud) is what both place their elements against

STICK_SENS has no entry: `romfs.c` overlay_init serves the player camera's pack with its speeds multiplied (camera_overlay, `camera_speed.c` cam_pack_build) on every game version, 1.1.1 included, and cam_speed_tick follows a change during the run. Whether the pack was found is known only once the RomFS is up (`romfs.c` fs_init), which on 1.1.1 is after `boot.c` cfg_log_unsupported first asks, and the log says so then ("camera speed: ...").

### `note_unsupported()`

One line per key the player set (`nx2navi.ini` or the environment) that this game build ignores.

## `runtime/cfg/cfg_probe.c`

--probe-cfg: `nx2navi.ini` parsing, the environment override, a missing file and the settings waiting for a restart (`cfg.c`). Needs no game files.

### `probe_cfg()`

Cfg_pending, before this probe's files replace the settings cfg_init read: a restart-only setting counts while its value differs from the one at start, a live one never

A value the environment sets after the start (the command line's --monitor arrives that way) never waits: no row can change it and a restart keeps it

A player's file as a report showed it: settings the menu wrote, and three lines added by hand that are no settings of this version (the player took the first for the fix of a crash). The three are named as such, a repeated one once, a key of other characters is counted without being listed, and the menu's save keeps every line it does not change byte for byte, the three included: only the changed, the reset and the new key move.

Cfg_set / cfg_save: only changed lines move, comments stay, a missing key goes under its section

Values cfg_load would trim or unquote: mod folder names in the order list start with a space or are quoted

Defaults table: the menu's "default" marks and Reset

DLSS frame generation's multiplier (`vk_dlssg.c`): the setting clamped to what the GPU and driver allow, with the reason vk_dlssg_frame logs when it is not taken as it is

Cfg_reset: the key's active lines leave `nx2navi.ini`, the commented example stays

An active line with an empty value has no entry in memory; Reset still removes it from the file

A file saved as UTF-16 (little and big endian, with a line ending of two characters) reads as the same lines

A key the environment sets is skipped and keeps its file line

A failed save: the call fails, cfg_last_save_ok says so, the value stays set and a later save works

Keyboard and mouse bindings (`input_map.c`)

Gamepad layouts (`input_map.c`)

Button prompts (`input_map.c`)

Romfs-patches-xbox shows A as B and romfs-patches-playstation A as circle (`tools/pad_prompts.py` FACE). That is right exactly where the layout puts A on the east button, which an Xbox pad labels B and a PlayStation pad circle.

The default layout is the Switch's own buttons by position, whatever the pad's labels (an Xbox pad's east button is labelled B and does Switch A): A right, B bottom, X top, Y left, L/R the bumpers, ZL/ZR the triggers.

Bind lines for stick directions and default parts; the remapping rows' edits (`input_map.c`)

- `input_map_set_pad(&mp, B_X, PAD_LAYOUT_XBOX, PAD_WEST);`: and back

Defaults live in the CfgMeta table: cfg_value and the typed accessors fall back to it

0.1.9's pad_layout = xbox_prompts (`cfg.c` migrate): read as the Nintendo layout with the Xbox prompts, saved so

Strtod: ranges such as 0.2..0.9 have fractional ends

Cfg_meta answers with the first row of a key: a second row would be dead, and Reset would visit the key twice

Every setting the menu lists has a section for cfg_save (a key without one would go to [misc], not where it belongs)

## `runtime/cfg/hw_detect.h`

The hardware the quality presets are chosen by (`hw_detect.c`; the choice itself: `preset_pick.h`).

### `hw_detect()`

Fills *hw from this PC: the GPU the game would use (a Vulkan probe of its own, see `hw_detect.c`), the processor's cores and logical processors, the installed memory and the handheld the firmware names. Called once, by `cfg.c` cfg_start once the settings are read and the guest address space is claimed. no_probe: why the caller leaves the Vulkan drivers alone (a tool mode, preset_probe = 0, a probe of an earlier start that did not finish), NULL or "" to probe. before_probe (may be NULL) runs right before the probe's first Vulkan call, only when the probe runs (`cfg.c` marks the state file with it). When the probe does not run, *skipped gets why (else ""), and the GPU fields stay empty for the caller to fill from the last probe's result; *note gets why a probe that ran found no GPU (else ""); *probe_ms the probe's time, the drivers' first load included.

The hardware the preset in use was chosen for (`cfg.c`: the probe's, or the last probe's from the state file). NULL before cfg_start. Never changes after it.

`vk_device.c` pick_device, once the game's own device is chosen: logs whether it is the GPU the preset was chosen for ("preset: the Vulkan device matches the probe" or "... differs from the probe: ...").

## `runtime/cfg/hw_detect.c`

The hardware the quality presets are chosen by (`cfg.c` cfg_start; the choice: `preset_pick.h`).

Design. The preset is decided inside cfg_start, before any restart-only setting is read and long before the game makes its Vulkan device, so the GPU is read by a probe of its own: a Vulkan instance without surface extensions, which lists the devices, picks the one `vk_device.c` pick_device would pick (NX2SARIA_GPU_INDEX or NX2SARIA_GPU_NAME, a gpu_index alone that lists a card that is not a discrete one counting as unset while a discrete one is usable (`gpu_choice.h`), else a discrete GPU before an integrated one before any other, a software renderer on the processor among the others; Vulkan 1.3 and the extensions the game needs; of one GPU listed by two drivers, the newer driver's listing), reads its name, IDs, driver version, type, largest device-local heap and mesh shader support, and is destroyed again. The window's present support cannot be asked without a window, so a GPU the game later refuses for that is the one case the probe and the game disagree on; `vk_device.c` says so in the log (hw_detect_device_check).

The probe is the process's first Vulkan call, so it first puts the drivers' cache variables in the environment (`vk_device.c` vk_driver_cache_env, which a driver reads when it loads). It does not run where something else must make the first Vulkan call: DLSS Frame Generation on Windows (framegen = dlss: Streamline's interposer, `vk_dlssg.c` vk_dlssg_early) and a RenderDoc capture (rdoc); nor in the tool modes (--selftest, --prepare-shaders and the others, cfg_start's game 0), which have no use for an early driver load; nor with preset_probe = 0, nor after a start whose probe never returned (`cfg.c` preset_init: the state file's probing mark). `cfg.c` then goes by the last probe's result, which it keeps in its state file. cfg_start runs once `memory.c` mem_init has claimed the whole guest address space, so the drivers' libraries and allocations land outside it, as they do for the game's own device later. The probe's time, the drivers' first load included, goes to the log each start.

The rest: logical processors (plat_cpu_count), physical cores (plat_cpu_cores), installed memory (plat_mem_info), and the handheld the firmware names (`preset_pick.h` preset_handheld; Linux's DMI tables, Windows' registry copy of the firmware's SMBIOS strings; also Steam's SteamDeck=1).

### `required_ext`

The device extensions `vk_device.c` pick_device requires (its k_required_ext): a GPU without one is not the game's.

### `usable_exts()`

1 when pd has every required extension; *mesh: VK_EXT_mesh_shader is listed.

### `has_graphics_queue()`

1 when pd has a queue family that does graphics and compute: the part of `vk_device.c` find_gfx_family that needs no window (whether that family presents cannot be asked here, as the design note above says).

### `probe_gpu()`

- `const char *e = cfg_get("NX2SARIA_GPU_INDEX");`: as `vk_device.c` pick_device: the best device when unset

A gpu_index that lists a card without gpu_name's name is dropped and the name decides, and a gpu_index alone that lists a card that is not a discrete one gives way to a discrete card the game can use, as in pick_device (`gpu_choice.h` gpuc_resolve): the preset has to be chosen for the card the game will use

Which discrete cards the game can use is asked only then, by the game's own tests but the one that needs a window (present support): a discrete card that cannot present is the case where the position gives way here and stands in the game, and hw_detect_device_check logs that the device differs from the probe.

- `if (p.apiVersion < VK_API_VERSION_1_3) continue;`: a CPU device counts, ranked with the others (as pick_device)
- `if (!usable_exts(pd[i], &mesh) || !has_graphics_queue(pd[i])) conti...`: as pick_device, but for the present test

Of one GPU listed by two drivers, the newer driver's listing (as pick_device)

- `if (best < 0 && (want >= 0 || (name && *name))) {`: a choice that is not usable: the best usable one, as the game does

The handheld the firmware names

### `bios_string()`

One string of the firmware's SMBIOS tables as Windows keeps them (HKLM\HARDWARE\DESCRIPTION\System\BIOS); "" when absent.

### `handheld()`

- `const char *env = getenv("SteamDeck");`: Steam sets it on the Steam Deck

### `hw_detect()`

- `vk_driver_cache_env();`: before the drivers first load: they read these variables then

### `hw_detect_device_check()`

- `cfg_preset_device_started();`: the drivers load: a probing mark of an earlier start can go

## `runtime/cfg/preset_pick.h`

Quality presets: which tier this PC gets, and what each tier sets. Header only and free of the settings code, so that `tests/preset_check.c` checks every decision with made-up hardware and no device.

Design. A preset is a layer of defaults in `cfg.c`: what the player sets (environment, `nx2saria.ini`) wins, then the device profile (`cfg.c` DECK, the Steam Deck), then the preset in use, then the settings table. `cfg.c` cfg_start reads the hardware once (`hw_detect.c` hw_detect), asks preset_pick for the tier this PC should get and puts the tier in use in that layer before any restart-only setting is read. "High" is the game as shipped and sets nothing; the other tiers set the keys of PRESET_ROWS below. A preset never turns frame generation on, lowers the frame-rate limit only on integrated GPUs, software renderers and handhelds, and asks for DLSS or FSR 4 only below High, on the cards that run them.

The tier: a handheld, an integrated GPU, a software renderer on the CPU or a GPU without mesh shaders (which the game's renderer then emulates) get Low; any other card goes by its video memory, up to High, then the PC's memory and processor count cap it. Without anything known about the GPU, the game as shipped (High). Ultra is never recommended: it only adds to the game as shipped, and on a large monitor it multiplies the pixels drawn (a 16 GB card on a 3840x2160 monitor would go from the 1920x1080 scene to a 3840x2160 one, about four times as many), so it stays the player's choice. Video memory is compared as the size the card is sold with: an 8 GB card reports 7.8 to 8.0 GiB of device-local memory, so it is rounded to the nearest GiB first (preset_gib). RAM goes by thresholds below the sold sizes (PRESET_RAM_*_MIB).

### Types

PRESET_GPU_CPU: a Vulkan device that renders on the processor (llvmpipe); OTHER: a virtual GPU or one of no known type

What the tier is chosen from. have_gpu 0: nothing is known of the GPU (no Vulkan 1.3 device, or the probe could not run and no earlier result was kept); the GPU fields are then 0. Any number 0 means unknown.

- `uint32_t vendor, device, driver;`: VkPhysicalDeviceProperties vendorID, deviceID, driverVersion
- `int gpu_type;`: PRESET_GPU_*
- `uint32_t vram_mib;`: the largest device-local memory heap
- `int mesh;`: VK_EXT_mesh_shader with both mesh and task shaders
- `int dlss;`: runs DLSS (preset_gpu_caps)
- `int fsr4;`: runs FSR 4 (preset_gpu_caps)
- `int cores, threads;`: physical cores and logical processors
- `uint32_t ram_mib;`: the memory the system reports installed
- `char handheld[32];`: the handheld's name (preset_handheld), "" for none

What each tier sets. One row per key and hardware; for one key, a later row that applies replaces an earlier one. Recalibrate from bench numbers here; `tests/preset_check.c` checks every value against its setting's choices. The rows keep to what costs GPU time measurably in this game version (`docs/presets.md`, Measurements): the scene's share, the picture's height and the filter that enlarges the scene. Anisotropic filtering, texture detail, foliage, anti-aliasing and the soft-shadow settings cost no time a desktop card shows, so High's and Medium's stay as shipped. Low lowers anisotropic filtering and foliage only on an integrated GPU, a software renderer or a handheld, whose memory bandwidth they may still ease; a card of its own that falls to Low (no mesh shaders, little video memory) keeps them, and no tier sets texture detail: its auto already picks smaller textures by the budget the driver grants. shadow_res has no effect on 1.1.1 (cfg_supported) and is in no tier.

- `PRESET_ANY,`: every PC
- `PRESET_SMALL,`: an integrated GPU, a software renderer on the processor (llvmpipe) or a handheld
- `PRESET_IGPU_DESK,`: one of those that is no handheld (a laptop or a desktop's processor graphics, llvmpipe)
- `PRESET_HANDHELD,`: a handheld
- `PRESET_DLSS,`: a card that runs DLSS (preset_gpu_caps)
- `PRESET_FSR4,`: a card that runs FSR 4 and not DLSS

### `PRESET_VRAM_LOW_GIB`

Tier boundaries, in GiB as sold (preset_gib): video memory under LOW gives Low, under MEDIUM Medium, from MEDIUM on High. RAM caps go by the MiB the system reports, below the sizes PCs are sold with, because a PC with integrated graphics hands part of its RAM to them: under RAM_LOW_MIB (an 8 GB PC) caps at Low, under RAM_MEDIUM_MIB (a 12 GB one) at Medium, and a 16 GB PC that reports 14 GiB or more is not capped. Up to CPU_MEDIUM logical processors cap at Medium.

### `preset_gpu_caps()`

DLSS: an NVIDIA card whose name says RTX (the GeForce RTX cards, the professional RTX ones). FSR 4: an AMD RDNA 4 card (Radeon RX 9070, 9060 and the others of the RX 90x0 series), on Windows only, where the FSR 4 library runs.

### `preset_handheld()`

The handheld PC the firmware names, NULL for none. vendor, product, family, board_vendor, board: the system's manufacturer, product name, product family, baseboard manufacturer and baseboard product, from Linux's /sys/class/dmi/id (sys_vendor, product_name, product_family, board_vendor, board_name) or Windows' registry (HKLM\HARDWARE\DESCRIPTION\System\BIOS: SystemManufacturer, SystemProductName, SystemFamily, BaseBoardManufacturer, BaseBoardProduct). Any of them may be NULL or empty.

Valve's Steam Deck: the LCD model Jupiter, the OLED model Galileo

ASUS ROG Ally (RC71L) and Ally X (RC72LA); the product name says "ROG Ally" and the board its model

Lenovo Legion Go: the family names it, the product name is the machine type (83E1 the first, 83L3 the Go S, 83N0 and 83N1 the Go 2)

MSI Claw

### `preset_pick()`

The tier this PC gets, and why in a few English words (into why, cap bytes).

### The table `PRESET_ROWS[]`

| Tier | Hardware | Key | Value | Note |
|---|---|---|---|---|
| Ultra | any | `SCENE_SHARE` | 1 | the 3D scene at the picture's full size (shipped: 0.75) |
| Ultra | any | `GAME_RES` | window | the picture and HUD at the window's height (shipped: 1440) |
| Ultra | any | `SHADOW_PCF` | 1 | shadow edges filtered over several samples |
| Medium | any | `SCENE_SHARE` | 0.67 | the 3D scene at two thirds of the picture |
| Medium | any | `GAME_RES` | 1080 | the picture and HUD 1080 lines high |
| Medium | DLSS | `DLAA`, `TAA`, `DLSS` | 1, dlss, quality | the scene through DLSS in its quality mode |
| Medium | FSR 4 | `DLAA`, `TAA`, `FSR4`, `DLSS` | 1, fsr3, 1, quality | the scene through FSR 4 in its quality mode |
| Low | any | `SCENE_SHARE` | 0.5 | the 3D scene at half the picture |
| Low | small | `SCENE_SHARE` | 0.4 | and less on the small ones (`PRESET_SMALL`) |
| Low | any | `GAME_RES` | 1080 | the picture and HUD 1080 lines high |
| Low | small | `ANISO` | 4 | 4x anisotropic filtering on the small ones |
| Low | small | `FOLIAGE` | reduced | fewer grass and bush draws there |
| Low | any | `STANDIN_FILTER` | bilinear | the scene enlarged with the plain filter (shipped: easu) |
| Low | small, no handheld | `FPS_LIMIT` | 30 | 30 fps, which an integrated GPU holds steadier |
| Low | handheld | `FPS_LIMIT` | 40 | 40 fps, which divides a handheld's 40, 80 and 120 Hz |
| Low | DLSS | `DLAA`, `TAA`, `DLSS` | 1, dlss, performance | the scene through DLSS in its performance mode |
| Low | FSR 4 | `DLAA`, `TAA`, `FSR4`, `DLSS` | 1, fsr3, 1, performance | the scene through FSR 4 in its performance mode |

Ultra is more than the game as shipped, and only the player chooses it (`preset_pick` never recommends it). High is the
game as shipped and has no rows.

### `preset_applies()`

A software renderer is the weakest case of all, so it gets an integrated GPU's values

### `preset_value()`

The value tier gives key (upper case, no prefix) on hw; NULL when the tier leaves it alone.

### `preset_fingerprint()`

The hardware as one line: vendor:device:driver:video GiB:logical processors:RAM GiB:handheld, the IDs in hex, both sizes rounded to the GiB as sold (preset_gib) and the handheld's name with '-' for blanks ("none" for none). The state file (`cfg.c`) keeps it. Video memory in whole GiB, as resizable BAR (Smart Access Memory) turned on or off or a driver update moves the heap the card reports by a few hundred MiB on the same card.

### `preset_fp_vram_gib()`

The video memory field of a fingerprint in GiB. A state file written before the fingerprint rounded it holds MiB there; a number of 256 or more is taken as MiB and rounded as preset_gib rounds (no card that runs the game has less than 256 MiB, and none has 256 GiB).

- `if (*end) return 0xffffffffu;`: not a number: equal only to itself (compared as text below)

### `preset_same_hardware()`

Whether two fingerprints name the same hardware: every field but the driver version, which a driver update changes on the same card; the video memory by its GiB (preset_fp_vram_gib), so a fingerprint of the older form in MiB still matches.
