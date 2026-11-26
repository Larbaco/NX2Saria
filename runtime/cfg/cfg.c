#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bugcap.h"
#include "cfg.h"
#include "cfg_private.h"
#include "discord_app.h"
#include "host.h"
#include "hw_detect.h"
#include "platform/platform.h"

#define OLD_PREFIX "NX2NAVI_"
#define OLD_PREFIX_LEN (sizeof OLD_PREFIX - 1)
#define PREFIX "NX2SARIA_"
#define PREFIX_LEN (sizeof PREFIX - 1)
#define CFG_MAX_KEYS 512

typedef struct { char *key, *value; int dirty, removed, held, test; } CfgEntry;

static CfgEntry entries[CFG_MAX_KEYS];
static int n_entries;
static unsigned version;
static PlatMutex set_lock = PLAT_MUTEX_INIT;
static uint64_t kept_n, kept_bytes;
static int save_ok = 1;

static const char *const known[] = {
    "HORSE_JUMP_FIX", "GOHMA_TURN_FIX", "FROZEN_ZORA_FIX", "BATTLE_MUSIC_FIX", "VOICE_LANGUAGE_FIX",
    "BALLOON_ANCHOR_FIX",
    "CORES", "TM_DISPATCH", "NVN2_CS_TABLE", "SKIP_SHADERS", "MOVIE_DEBAND", "MOVIE_FPS", "MOVIE_SKIP", "MOVIE_OVERLAY", "MOVIES", "NPLN", "NVN2_PROGRAMS", "NVN_TRAP", "OCARINA_REALTIME", "ONESHOT_BUDGET", "ONESHOT_WAIT_MS", "OCARINA_STEP", "PHYSICS_STEPS", "PHYSICS_STEPS_FULL", "PICKUP_COUNT", "SONG_STAFF", "WATER_STEPS", "SOUND_STEPS", "FLOOR_PROBE", "FLOOR_SCAN", "FLOOR_WATCH", "FLOOR_CARRY", "MOVIE_PTS", "STEP_STEADY", "VI_HZ", "VI_VRR", "TIME_DUMP", "SHADER_DUMP", "SHOT_AFTER", "SHOT_DIR", "SHOT_EVERY",
    "OCARINA_TIMING",
    "FSR3_PROBE_PAN", "FSR3_PROBE_PPM",
    "FSR3FG_PROBE_PPM", "FSR3FG_PASS_TIMES", "FSR3FG_DUMP", "FSR3FG_DUMP_EVERY", "FSR3FG_TEST_FAIL",
    "FG_GPU_TIME", "DLAA_TEST_DEFERRED",
    "AA", "AA_LOG", "AB", "AMIIBO_DAILY_LIMIT", "AMIIBO_DIR", "AMIIBO_KEYS", "AMIIBO_POPUP", "AMIIBO_WRITEBACK", "ANISO", "APP_POOL", "ASPECT", "ASPECT_PATCH", "ASSUME_FOCUS", "ASTC", "ASTC_BC", "ASTC_TIME", "ASYNC_PREEMPT", "AUDIO", "AUDIO_CATCHUP", "AUDIO_CHANNELS", "AUDIO_DEVICE", "AUDIO_GAME_PRIO", "AUDIO_OFF", "AUDIO_PREFILL_MS", "AUDIO_RAMP", "AUDIO_STATS",
    "AUDIO_STRICT",
    "BARRIER_GAME", "MESH_AHEAD", "MESH_FALLBACK", "MESH_FALLBACK_MB", "MESH_FALLBACK_WORKERS", "MESH_FALLBACK_HOLD", "MESH_FALLBACK_BATCH", "PROBE_MESHFB_DIR",
    "CAPTURE_AT", "CAPTURE_DIR", "CAPTURE_REPLAY", "CAPTURE_REPLAY_CAMERA",
    "BOOT_MAP", "BOOT_MAP_SLOT", "BOOT_MAP_AGE", "BOOT_MAP_TIME",
    "BACKGROUND_WINDOW", "BIND_A", "BIND_B", "BIND_DOWN", "BIND_L", "BIND_LCLICK", "BIND_LEFT", "BIND_LSTICK_DOWN", "BIND_LSTICK_LEFT",
    "BIND_LSTICK_RIGHT", "BIND_LSTICK_UP", "BIND_MINUS",
    "BIND_PLUS", "BIND_R", "BIND_RCLICK", "BIND_RIGHT", "BIND_RSTICK_DOWN", "BIND_RSTICK_LEFT", "BIND_RSTICK_RIGHT", "BIND_RSTICK_UP",
    "BIND_SHAKE",
    "BIND_SKIP", "BIND_STATS", "BIND_UP", "BIND_X", "BIND_Y",
    "BIND_ZL", "BIND_ZR", "BLOCK_VIEWS_3D", "CACHE_DIE", "CLOTH_REALTIME", "COLOR_BRIGHTNESS", "COMPUTE_KEEP", "COLOR_CONTRAST", "COLOR_GAMMA",
    "COLOR_PRESET", "COLOR_SATURATION", "CONTACT_DEBUG", "CONTACT_SHADOWS", "CORE_DIAG", "CRIT", "CRIT_OUT",
    "DMABUF", "DMABUF_MB", "DRAW_CRUMBS", "GPA_LATE", "DESC_TEMPLATE", "DEVICE_PROFILE", "DISCORD_APP_ID", "DISCORD_IMAGE", "DISCORD_RPC", "DLAA", "DLAA_CAM_KEEP", "DLAA_CUT", "DLSS_IDLE_FREE", "DLAA_EDGE_FILL", "DLAA_AT_UPSCALER", "FFX_DEBUG", "FFX_KEYS", "FSR3_SHARPEN", "DLAA_JITTER", "DLAA_JITTER_TEST", "DLSS_LOD_BIAS_TEST", "DLAA_MV", "DLAA_MASKDEBUG", "DLAA_MVDEBUG", "DLAA_OBJMV", "DLAA_SURVEY", "DLAA_TARGET_KEEP", "DLAA_TIMES", "DLSS", "DLSS_CHAR_MASK", "DLSS_PRESET", "DLSS_PREBLOOM", "DLSS_RANGE", "DLSS_REACTIVE", "DUMP", "DUMP_BUF", "DUMP_DIR",
    "UPSCALER_IDLE_WAIT",
    "DUMP_LOG", "DUMP_RAW", "DUMP_SHADERS", "DUMP_FILE", "DS3", "LOG_CS_BINDS", "LOG_TEX_FLAGS", "LOG_MESH", "MESH", "POOL", "NATIVE_TENSOR", "LOG_RCOND", "LOG_RT_SIZES", "DUMP_WHITE", "DUMP_AFTER", "DUMP_MOVIE", "SUBMIT_FINISH", "ACQUIRE_WAIT", "CRASH_FILTER_GUARD", "CS_FALLBACK", "EXEFS", "FEEDBACK", "FIX_C0", "FIX_ZERO", "FLIGHT", "FLIGHT_AT", "FLIGHT_DIR", "FLIGHT_FRAMES", "GPU_TIMELINE",
    "FFX_PIPE_CACHE", "FLIGHT_SPIKE_MS", "FREEZE_QUIET_MS", "FREEZE_LOAD_HOLD_MS", "STUTTER_MAP", "PAR_JOBS", "PIPE_RELEASE", "FLIGHT_THUMBS", "FLIGHT_EVERY", "FLIGHT_FLASH", "FLIGHT_FLASH_DIFF", "FLIGHT_FLASH_CELLS", "FLUSH_SPAN", "FLUSH_UPLOAD", "FOG_CULL", "FOLIAGE_SHADOW_CULL", "FOLIAGE_SHADOW_VRS", "FOV", "FOV_PATCH", "FG_HUDLESS_SHOW", "FG_MULT", "FPS60", "FPS_LIMIT", "FRAMEGEN", "FRAMEGEN_MULTIPLIER", "FREEZE_IDLE", "FREEZE_ASYNC", "FREEZE_BUDGET_US", "FREE_CORES",
    "FULLSCREEN", "FXAA", "GAME_ASPECT", "GAME_RES", "WIDE_HUD", "WIDE_MENUS", "GPA", "GPA_EMPTY", "GPA_TWIN", "GPA_TWIN_MAX_KB", "MIRROR_CAPTURE_KB", "WB_SKIP_MB", "GPL", "GPL_DRAW_BUILD", "GPL_FEEDBACK", "GPL_MESH", "PIPE_AHEAD", "PIPE_FAST_WORKERS", "PIPE_FRAME_WAIT_MS", "PIPE_LINK_DRAW_MS", "PIPE_SKIP_STEADY", "DRAW_WRITERS_WAIT", "PROG_LEAD", "GPU", "GPU_INDEX", "GPU_PASSES", "GPU_NAME", "HDR",
    "HDR_GAME_NITS", "HDR_LIVE", "HEAP_LAZY", "HDR_PEAK_NITS", "HDR_UI_NITS",
    "HDR_BLOWOUT", "HDR_CONTRAST", "HDR_EXPOSURE", "HDR_FLARE", "HDR_GAMMA", "HDR_GRADE", "HDR_HIGHLIGHT_SAT",
    "HDR_HIGHLIGHTS", "HDR_HUE", "HDR_HUE_CLIP", "HDR_MATCH_SLOPE", "HDR_MIDGRAY", "HDR_SATURATION", "HDR_SHADOWS",
    "HDR_TONEMAP", "HDR_TONE_SLOPE", "HIGHFPS_FIX", "INPUT", "INPUT_RECORD", "INPUT_SCRIPT", "LANG", "LIB_PREBUILD",
    "BIG_BUILD_FREE_MB", "BIG_BUILD_MAX_MB", "BG_BUILD_PLAY_DRAWS", "BUILD_FREE_MB", "BUILD_RESERVE_MB", "LIB_PREBUILD_BIG", "LIB_PREBUILD_BIG_BG", "COMPUTE_WARM_REUSE", "LIB_PREBUILD_KEEP", "LIB_PREBUILD_THREADS", "LIB_WARM", "PREPARE_STALL_S", "PREPARE_STALL_TEST_MS", "LINE_WIDTH", "POOL_SHOWN_FIRST", "POOL_TEX_RELEASE", "POOL_DATA_FIRST", "TEX_REUSE_NAMED", "TEX_OVERWRITTEN_USED", "PIPE_LINK_MS", "FRAME_HOLD_MS", "PACE_SMOOTH", "OQ_BATCH", "OQ_HOLD", "QUAD_INDEX", "CLEAR_SMALL_CPU", "GATE_WAIT_MS", "LOCALE", "LOD_GUARD", "LSTICK_DEADZONE", "MEMORY", "MEM_CENSUS", "MEM_NEED_MB", "MENU_LANG", "MENU_SOUND_VOLUME", "MENU_TEST",
    "MENU_CURSOR", "MENU_CURSOR_LOG", "MENU_MOUSE_PATCH", "MENU_TEST_CLICK", "MENU_TEST_DIR", "MENU_TEST_SET", "MINIMAL_LAYOUT", "MICROPHONE", "MIRROR_GAP", "MODS_DIR", "MODS_DISABLED", "MODS_ORDER", "MODS_REBUILD", "MONITOR", "MOTION", "MOUSE_MODE", "MOUSE_DIRECT_PATCH", "MOUSE_SENS", "MOUSE_TRACE", "MOVIE_HOST", "MOVIE_PACE", "MOVIE_CLOCK", "TEST_HITCH", "SPRINT_BUTTON", "SPRINT_MODE", "SPRINT_BUTTON_PATCH", "TEXT_SPEED", "TEXT_SPEED_PATCH", "AMIIBO_DAILY_PATCH",
    "MOTION_GUARD_DEGREES", "MOTION_POLICY_TRACE", "SELECTIVE_MOTION", "SELECTIVE_MOTION_RAW",
    "SKIN_MOTION",
    "GYRO_CALIBRATION", "GYRO_DEADZONE", "GYRO_TURN_SENS", "GYRO_TILT_SENS", "GYRO_TURN_INVERT", "GYRO_TILT_INVERT", "TEST_PAD_GYRO",
    "TRACKER_API", "TRACKER_API_PORT",
    "MVOBJ_ALL", "MVOBJ_BANKS", "MVOBJ_EQUAL", "MVOBJ_EXACT", "MVOBJ_EXEC", "MVOBJ_NOC4", "MVOBJ_ONLY", "MVOBJ_TIMES", "MVPREV_CHECK", "MVPREV_GROW", "MVPREV_SKIP", "MVPREV_TRUST", "MV_CHECK",
    "MV_SURVEY",
    "HANDLE_CENSUS",
    "PIPE_SEED_BOOT", "ONE_PER_CORE", "OPERATION_MODE", "OUTPUT_RES", "PAD_LAYOUT", "PACE_HZ", "PASS_CRUMBS", "PATCH_OUTPUT", "PATCH_RES_FIXED", "PATCH_SCENE", "PATCH_WIDE_ASPECT", "PATCH_WIDE_HUD", "PATCH_WIDE_MOVIE", "PATCH_WIDE_OUT", "PATCH_FREECAM", "PATCH_OCCLUSION_OFF", "PATCH_WIDE_SAVE", "PATCH_WIDE_SCENE", "PATCH_WIDE_BARS", "PCSS", "PCSS_K", "PERF_OVERLAY", "PERF_SLOW_MS", "PIPELINE_CACHE", "PIPELINE_CACHE_MAX_MB", "PIPELINE_CACHE_PLAY", "PIPELINE_CACHE_PLAY_MB", "PIPELINE_CACHE_STAMP", "PIPE_BINARIES", "PIPE_BINARIES_MAX_MB", "PIPE_FEEDBACK", "PIPE_MEMO", "PIPE_PREBUILD", "PIPE_SEED", "PIPE_SKIP", "PIPE_SYNC", "PIPE_WAIT", "POWER_FULL", "TEX_SAME", "TEX_SAME_BIG", "TEX_FLAT3D", "TEX_KEEP_TAKEN", "TEX_RATE_LOW", "TEX_HARD_ORDER", "TEX_STALE_KEPT", "ASTC_SCRATCH_TRIM", "PRESENT_HELD", "HELD_SYNC",
    "PRESENT_ORDER",
    "PREPARE_PIPELINES", "PREWARM_FULL", "PREWARM_FULL_S", "PREWARM_WAIT_MS", "PREWARM_XLATE", "PROBE_MOVIE", "PROBE_MOVIE_SHOT", "PROFILE", "PROFILE_CHAINS", "PROFILE_DIR", "PROFILE_FRAMES", "PROFILE_REPLAY", "PROFILE_SLOW",
    "POOL_FAST", "POOL_GROUP", "POOL_GROUP_CHECK", "POOL_PRESNAP", "POOL_PRESNAP_RUNS",
    "SUBMIT_FINISH_GPU", "SVC_WAITS", "SNAP_WIDEN", "SUBMIT_FINISH_RESULTS", "SUBMIT_FINISH_KINDS", "FOLIAGE", "MIRROR_STAGE", "FLUSH_SKIP_CLEAN", "MIRROR_HOT", "MESH_EMULATION", "MESH_EMU_MB", "MESH_EMU_TASKS", "MESH_EMU_BATCH", "GUEST_OPT", "LOG_TEX_NEW", "DUMP_KEY", "VEC_INDEX_FIX", "PASS_TIMES",
    "PROFILE_MAIN", "PROFILE_RENDER", "PROMPTS", "PUSH_SKIP", "PUSH_STAGE", "RDOC", "RDOC_DIR", "RDOC_DLL", "REALTIME", "REC_END", "REEXEC",
    "REFLEX", "REFLEX_LIMIT", "LOW_LATENCY", "RENDER_SCALE", "REPLAY_ASYNC", "REPLAY_PIPE", "REPLAY_RUNS", "REPLAY_SMALL", "TAIL_EARLY", "REPLAY_WORKERS", "RTSS", "RTSS_ENUM", "SCENE_SHARE",
    "PRESNAP_PLACE",
    "REPLAY_CHECK", "RESOLVE_CHECK", "RESTART_WAIT", "ROMFS", "ROMFS_INDEX", "ROMFS_MMAP", "ROMFS_MODS", "ROMFS_PATCHES",
    "ROMFS_PROOF_DIR", "ROMFS_VERIFY", "RSTICK_DEADZONE", "RUMBLE", "RUMBLE_CURVE", "RUMBLE_HD", "RUMBLE_HD_MS", "RUMBLE_STRENGTH", "RUMBLE_TRIGGERS", "SHADER_CACHE",
    "SHADER_CACHE_FLUSH", "SHADER_CARRY", "SHADER_COMPRESS", "SHADER_COMPRESS_IDLE_S", "SHADER_MEMO", "SHADOW_BULK_WRITE", "SHADOW_CHECK", "SHADOW_COMPARE", "SHADOW_PCF", "SHADOW_RANGE", "SHADOW_RES", "SHADOW_SCALE", "SHADOW_TABLE_STORES", "SHARED_FONTS", "SHARPNESS", "SHOTS", "SHOTS_DIR",
    "SVC_SLOW_MS",
    "SHOTS_FULL", "SPEED_PROBE", "SPIN", "SPIN_HANDOFF_US", "SPLIT_DRAWS", "SPLIT_IDLE", "SPLIT_IN_PASS", "SPLIT_US", "SPV_MARK_TEST", "SPV_OVERRIDE", "STICK_DEADZONE", "STICK_SENS",
    "SWKBD_ANSWER",
    "SET_AT", "SWKBD_TEST", "TAA", "DBG_TEST", "DEBUG_MENU", "MENU_TEST_DEBUG", "FSR4", "FSR4_FG", "FG4_VERSION", "FG4_SWAPCHAIN", "TEST_PAD", "TEST_PAD_AT", "TEST_NO_UFFD", "TEST_PREPARE_SHADERS", "TEX_ALIAS", "DEPTH_INIT_INLINE", "DEPTH_ALIAS_LOG", "TEX_DISUSE", "POOL_MARK_SHARED", "TEX_DISUSE_EVICT", "TEX_GEN_UNDO", "TEX_MEMO", "TEX_VRAM_PRESSURE", "TEX_RELEASED_MB", "TEX_RESIDENCY", "TEX_RELEASED_MAX", "TEX_RELEASED_HOLD", "TEX_REUSE", "TEX_SPLIT_FILL", "TEX_COMMIT_FREE_MB", "TEX_PROC_COMMIT_MB", "TEX_EVICT", "TEX_COMMIT_EVICT", "MIRROR_EVICT", "VRAM_BUDGET_MB", "TIMER_1MS", "TRACE", "TXD_TRANSPOSE", "UBO_SNAPSHOT", "EVO_TRACE", "EVENT_AT_SYNC", "UPSCALER", "VK_DYN_A2C", "VK_DYN_VERTEX", "VK_NAN_PRESERVE", "VK_NOOPT", "VK_NO_FLOAT64",
    "VK_PRESENT", "VK_RECONVERGE", "VK_NAN_BITS", "VK_RECORDER", "VK_SSBO_ALIGN", "VK_SUBGROUP", "VK_SUBGROUP_PIN", "VK_MESH_SUBGROUPS", "VK_MESH_INVARIANT", "VK_VALIDATION", "VOLUME", "VRAM_HEADROOM", "VS_OUTS", "WATER_PROJECTIVE",
    "WAIT_WATCH_S", "WALK_KEY", "WALK_SCALE", "WB_EXACT", "WB_FENCE_LIMIT", "WB_MERGE", "WB_NO_REUP", "WB_MERGE_MAX", "WB_PROBE", "WB_SPLIT", "WB_LAZY", "WB_LAZY_CHUNK", "WB_LAZY_TOUCH", "WB_LAZY_UNLOCKED", "WINDOW_SIZE", "XLATE", "XLATE_ID", "XLATE_KEEP", "XLATE_PROCS",
    "UPLOAD_BATCH_MB", "VRAM_HEAP_LIMIT", "VRAM_PAGEABLE", "VRAM_RESERVE_MB", "TEX_CLOBBER", "TEX_OVERSIZE_ALIAS", "LOW_END_GPU",
    "ZSTD_CHECK", "ZSTD_NATIVE",
    "BIND_REUSE", "DYN_DELTA", "STG_ARENA", "SUBMIT_BATCH", "SUBMIT_THREAD", "ASTC_GPU_DESWIZZLE", "ASTC_SCRATCH_KEEP", "ASTC_ARENA", "ASTC_BC1",
    "MERGE_CPU", "MERGE_CPU_CHECK", "MERGE_CPU_MIN", "WB_DIFF",
    "TEXUP_LOG_MS", "DYN_RES", "DYN_RES_TARGET", "DYN_RES_STEP", "DYN_RES_MIN", "TEXTURE_DETAIL", "VRAM_USE",
    "RECORDER_CORE", "RECORDER_PRIO", "READABLE_FAST", "REPLAY_TIME", "REPLAY_PHASES", "LOCK_TIME", "POOL_CREATE_BUDGET", "POOL_CREATE_MS", "POOL_FINI_IDLE", "POOL_LOAD_BACKLOG", "POOL_LOAD_MS",
    "VMA_BLOCK_MB", "SYNC_WAKE_ALL", "LOG_ASYNC", "LOG_SHORT_PATHS", "DEVICE_LOST_AT", "STAGING_FAIL_AT", "VRAM_BALLAST_MB",
    "GEN_SEED",
    "COPY_RETIRE",
    "PRESET",
    "PRESET_PROBE",
    "NVIDIA_CACHE_DIR",
    "MEM_BUDGET_TEST_VRAM_MB", "MEM_BUDGET_TEST_RAM_MB",
    "VRAM_WARNING",
    "TEX_COMPACT", "TEX_COMPACT_MIN_MB", "TEX_COMPACT_FRAME_MB",
    "STANDIN_FILTER",
    "AUDIO_DUMP", "DEFER_STAMP", "DEPTH_TWIN", "DLAA_ZFIT", "DUMP_SKIP_RUNS", "DUMP_TEX", "DUMP_UBO", "EVENT_ORDER",
    "GATHER_BIAS", "PIN_SWEEP", "REC_CHECK", "REC_DELAY_US", "REG_ORDER", "SHADOW", "ZERO_LOCALS",
    "HEAP_RETRY", "HEAP_RETRY_TEST_FAIL", "HEAP_RETRY_TEST_SHIPPED",
    "CHANGEBANK_HOLD", "CHANGEBANK_HOLD_MS",
    "CAMERA_CLEARANCE_FIX",
    "QUIT_MENU", "QUIT_MENU_TEST",
    "POS_TRACE",
    "HANG_WATCH_S",
    "COOP", "COOP_BETA", "COOP_NAME", "COOP_SERVER", "COOP_PORT", "COOP_ROOM", "COOP_AUTO", "COOP_DROP",
    "COOP_PROGRESS", "COOP_ITEMS", "COOP_SONGS", "COOP_QUEST", "COOP_KEYS", "COOP_RUPEES", "COOP_AMMO",
    "COOP_TEST_SET", "COOP_ACCEPT", "COOP_REMOVE", "COOP_RESET_POOLS",
    "COOP_STEAM", "COOP_SHARE", "COOP_NAMES", "COOP_CHAT",
};

static const struct { const char *key, *section; } sections[] = {
    {"TEX_RELEASED_MAX", "performance"}, {"MIRROR_HOT", "performance"}, {"FREEZE_IDLE", "performance"}, {"SUBMIT_FINISH_KINDS", "performance"}, {"FOLIAGE", "performance"},
    {"ZSTD_NATIVE", "performance"},
    {"VRAM_HEAP_LIMIT", "performance"}, {"VRAM_RESERVE_MB", "performance"}, {"UPLOAD_BATCH_MB", "performance"}, {"MESH_EMULATION", "performance"},
    {"MESH_EMU_MB", "performance"}, {"MESH_EMU_TASKS", "performance"}, {"MESH_EMU_BATCH", "performance"}, {"VEC_INDEX_FIX", "performance"},
    {"GUEST_OPT", "performance"}, {"PASS_TIMES", "diagnostics"}, {"DUMP_KEY", "diagnostics"},
    {"FLOOR_PROBE", "diagnostics"}, {"FLOOR_SCAN", "diagnostics"}, {"FLOOR_WATCH", "diagnostics"},
    {"LOG_SHORT_PATHS", "diagnostics"},
    {"FPS_LIMIT", "frame rate"}, {"FPS60", "frame rate"}, {"RTSS", "frame rate"},
    {"REALTIME", "frame rate"}, {"CLOTH_REALTIME", "frame rate"}, {"HIGHFPS_FIX", "frame rate"},
    {"HORSE_JUMP_FIX", "frame rate"}, {"GOHMA_TURN_FIX", "frame rate"}, {"FROZEN_ZORA_FIX", "frame rate"},
    {"BATTLE_MUSIC_FIX", "frame rate"},
    {"PHYSICS_STEPS", "frame rate"}, {"PICKUP_COUNT", "frame rate"}, {"WATER_STEPS", "frame rate"},
    {"SOUND_STEPS", "frame rate"}, {"SONG_STAFF", "frame rate"}, {"FLOOR_CARRY", "frame rate"}, {"OCARINA_TIMING", "frame rate"},
    {"MOVIE_PTS", "frame rate"}, {"STEP_STEADY", "frame rate"},
    {"FULLSCREEN", "display"}, {"WINDOW_SIZE", "display"}, {"PERF_OVERLAY", "display"}, {"VK_PRESENT", "display"},
    {"GPU_INDEX", "display"}, {"GPU_NAME", "display"}, {"MONITOR", "display"}, {"OUTPUT_RES", "display"}, {"GAME_ASPECT", "display"}, {"GAME_RES", "display"}, {"WIDE_HUD", "display"}, {"WIDE_MENUS", "display"}, {"BALLOON_ANCHOR_FIX", "display"}, {"RENDER_SCALE", "display"}, {"UPSCALER", "display"},
    {"SHARPNESS", "display"}, {"STANDIN_FILTER", "display"}, {"SCENE_SHARE", "display"}, {"ASPECT", "display"}, {"FOV", "display"}, {"ANISO", "display"},
    {"TEXTURE_DETAIL", "display"},
    {"COLOR_PRESET", "display"},
    {"COLOR_BRIGHTNESS", "display"}, {"COLOR_CONTRAST", "display"}, {"COLOR_SATURATION", "display"},
    {"COLOR_GAMMA", "display"}, {"HDR", "display"}, {"HDR_LIVE", "display"}, {"HDR_PEAK_NITS", "display"}, {"HDR_GAME_NITS", "display"},
    {"HDR_UI_NITS", "display"}, {"HDR_GAMMA", "display"}, {"HDR_TONEMAP", "display"}, {"HDR_EXPOSURE", "display"},
    {"HDR_HIGHLIGHTS", "display"}, {"HDR_SHADOWS", "display"}, {"HDR_CONTRAST", "display"}, {"HDR_SATURATION", "display"},
    {"HDR_HIGHLIGHT_SAT", "display"}, {"HDR_BLOWOUT", "display"}, {"HDR_FLARE", "display"}, {"HDR_HUE", "display"},
    {"HDR_GRADE", "display"}, {"HDR_MIDGRAY", "display"}, {"HDR_MATCH_SLOPE", "display"}, {"HDR_TONE_SLOPE", "display"},
    {"HDR_HUE_CLIP", "display"}, {"SHADOW_RES", "display"},
    {"DLAA", "display"}, {"FSR4", "display"}, {"FSR4_FG", "display"}, {"DLSS", "display"}, {"DLSS_PRESET", "display"}, {"DLSS_IDLE_FREE", "display"}, {"DLSS_RANGE", "display"}, {"DLSS_PREBLOOM", "display"}, {"DLSS_CHAR_MASK", "display"}, {"FXAA", "display"}, {"AA", "display"},
    {"UPSCALER_IDLE_WAIT", "display"},
    {"FRAMEGEN", "display"}, {"FRAMEGEN_MULTIPLIER", "display"}, {"FG_MULT", "display"}, {"SKIN_MOTION", "display"},
    {"LOW_LATENCY", "display"}, {"REFLEX_LIMIT", "display"},
    {"DEVICE_PROFILE", "display"}, {"ASSUME_FOCUS", "display"}, {"PRESET", "display"}, {"PRESET_PROBE", "display"},
    {"DISCORD_RPC", "discord"}, {"DISCORD_APP_ID", "discord"}, {"DISCORD_IMAGE", "discord"},
    {"TRACKER_API", "tracker"}, {"TRACKER_API_PORT", "tracker"},
    {"SHADOW_SCALE", "display"}, {"SHADOW_RANGE", "display"}, {"CONTACT_SHADOWS", "display"},
    {"PCSS", "display"}, {"TAA", "display"}, {"SHADOW_COMPARE", "display"},
    {"SHADOW_PCF", "display"},
    {"AMIIBO_DAILY_LIMIT", "amiibo"}, {"AMIIBO_DIR", "amiibo"}, {"AMIIBO_KEYS", "amiibo"}, {"AMIIBO_POPUP", "amiibo"}, {"AMIIBO_WRITEBACK", "amiibo"},
    {"MODS_DIR", "mods"}, {"MODS_ORDER", "mods"}, {"MODS_DISABLED", "mods"}, {"MODS_REBUILD", "mods"},
    {"MENU_LANG", "language"}, {"LANG", "language"}, {"OPERATION_MODE", "language"}, {"VOICE_LANGUAGE_FIX", "language"}, {"TEXT_SPEED", "language"},
    {"VOLUME", "audio"}, {"AUDIO_CHANNELS", "audio"}, {"AUDIO_DEVICE", "audio"}, {"MICROPHONE", "audio"}, {"MENU_SOUND_VOLUME", "audio"},
    {"AUDIO_RAMP", "audio"}, {"AUDIO_CATCHUP", "audio"},
    {"AUDIO_STRICT", "audio"},
    {"MOTION", "input"}, {"SPRINT_BUTTON", "input"}, {"SPRINT_MODE", "input"}, {"RUMBLE_STRENGTH", "input"}, {"RUMBLE_CURVE", "input"}, {"RUMBLE_TRIGGERS", "input"}, {"RUMBLE_HD", "input"}, {"DS3", "input"},
    {"PAD_LAYOUT", "input"}, {"PROMPTS", "input"}, {"STICK_DEADZONE", "input"}, {"LSTICK_DEADZONE", "input"}, {"RSTICK_DEADZONE", "input"},
    {"GYRO_CALIBRATION", "input"}, {"GYRO_DEADZONE", "input"}, {"GYRO_TURN_SENS", "input"}, {"GYRO_TILT_SENS", "input"}, {"GYRO_TURN_INVERT", "input"}, {"GYRO_TILT_INVERT", "input"},
    {"STICK_SENS", "input"}, {"DEBUG_MENU", "input"}, {"AMIIBO_DIR", "files"},
    {"MENU_CURSOR", "input"}, {"MOUSE_MODE", "input"}, {"MOUSE_SENS", "input"}, {"WALK_KEY", "input"}, {"WALK_SCALE", "input"}, {"INPUT_RECORD", "input"},
    {"BIND_A", "input"}, {"BIND_B", "input"}, {"BIND_X", "input"}, {"BIND_Y", "input"}, {"BIND_LCLICK", "input"},
    {"BIND_RCLICK", "input"}, {"BIND_L", "input"}, {"BIND_R", "input"}, {"BIND_ZL", "input"}, {"BIND_ZR", "input"},
    {"BIND_PLUS", "input"}, {"BIND_MINUS", "input"}, {"BIND_LEFT", "input"}, {"BIND_UP", "input"},
    {"BIND_RIGHT", "input"}, {"BIND_DOWN", "input"}, {"BIND_LSTICK_LEFT", "input"}, {"BIND_LSTICK_UP", "input"},
    {"BIND_LSTICK_RIGHT", "input"}, {"BIND_LSTICK_DOWN", "input"}, {"BIND_RSTICK_LEFT", "input"},
    {"BIND_RSTICK_UP", "input"}, {"BIND_RSTICK_RIGHT", "input"}, {"BIND_RSTICK_DOWN", "input"}, {"BIND_SHAKE", "input"},
    {"ASYNC_PREEMPT", "performance"}, {"REPLAY_ASYNC", "performance"}, {"REPLAY_WORKERS", "performance"}, {"REPLAY_PIPE", "performance"}, {"TAIL_EARLY", "performance"}, {"REPLAY_SMALL", "performance"},
    {"FREE_CORES", "performance"},
    {"DYN_RES", "performance"}, {"DYN_RES_TARGET", "performance"}, {"DYN_RES_STEP", "performance"}, {"DYN_RES_MIN", "performance"},
    {"RECORDER_CORE", "performance"}, {"RECORDER_PRIO", "performance"}, {"READABLE_FAST", "performance"},
    {"BIND_REUSE", "performance"},
    {"DYN_DELTA", "performance"}, {"STG_ARENA", "performance"}, {"SUBMIT_BATCH", "performance"}, {"SUBMIT_THREAD", "performance"},
    {"NVIDIA_CACHE_DIR", "performance"},
    {"BUILD_FREE_MB", "performance"}, {"BG_BUILD_PLAY_DRAWS", "performance"}, {"BUILD_RESERVE_MB", "performance"}, {"MEM_NEED_MB", "diagnostics"},
    {"ASTC_GPU_DESWIZZLE", "performance"}, {"ASTC_SCRATCH_KEEP", "performance"}, {"ASTC_ARENA", "performance"}, {"ASTC_BC1", "performance"}, {"POOL_CREATE_BUDGET", "performance"},
    {"POOL_CREATE_MS", "performance"}, {"POOL_FINI_IDLE", "performance"}, {"FREEZE_BUDGET_US", "performance"}, {"FREEZE_QUIET_MS", "performance"}, {"FREEZE_LOAD_HOLD_MS", "performance"}, {"POOL_LOAD_BACKLOG", "performance"}, {"POOL_LOAD_MS", "performance"},
    {"TIMER_1MS", "performance"}, {"PIPE_WAIT", "performance"}, {"PIPE_SKIP", "performance"}, {"ONESHOT_BUDGET", "performance"},
    {"PIPE_FRAME_WAIT_MS", "performance"}, {"PIPE_SKIP_STEADY", "performance"}, {"DRAW_WRITERS_WAIT", "performance"},
    {"CACHE_DIE", "performance"}, {"ONE_PER_CORE", "performance"},
    {"PIPE_SEED_BOOT", "performance"}, {"LIB_PREBUILD", "performance"}, {"LIB_PREBUILD_THREADS", "performance"}, {"LIB_WARM", "performance"}, {"PREPARE_STALL_S", "performance"}, {"PREPARE_STALL_TEST_MS", "diagnostics"}, {"PIPE_LINK_MS", "performance"}, {"PIPE_LINK_DRAW_MS", "performance"}, {"PIPE_AHEAD", "performance"}, {"LINE_WIDTH", "performance"}, {"POOL_SHOWN_FIRST", "performance"}, {"POOL_DATA_FIRST", "performance"}, {"TEX_REUSE_NAMED", "performance"}, {"TEX_OVERWRITTEN_USED", "performance"}, {"PIPE_FAST_WORKERS", "performance"}, {"GPL_FEEDBACK", "performance"}, {"GPL_MESH", "performance"}, {"FRAME_HOLD_MS", "performance"}, {"PACE_SMOOTH", "performance"}, {"OQ_BATCH", "performance"}, {"CLEAR_SMALL_CPU", "performance"}, {"PATCH_OCCLUSION_OFF", "performance"}, {"GATE_WAIT_MS", "performance"}, {"PIPE_PREBUILD", "performance"}, {"PIPE_SEED", "performance"},
    {"OQ_HOLD", "performance"}, {"QUAD_INDEX", "performance"},
    {"COMPUTE_KEEP", "performance"},
    {"LIB_PREBUILD_BIG_BG", "performance"}, {"PIPE_FEEDBACK", "performance"}, {"PIPELINE_CACHE_MAX_MB", "performance"}, {"PIPE_BINARIES", "performance"}, {"PIPE_BINARIES_MAX_MB", "performance"}, {"ONESHOT_WAIT_MS", "performance"}, {"BIG_BUILD_FREE_MB", "performance"}, {"BIG_BUILD_MAX_MB", "performance"}, {"ZSTD_CHECK", "performance"}, {"VK_NOOPT", "performance"},
    {"PREWARM_XLATE", "performance"}, {"GPL", "performance"}, {"ASTC", "performance"}, {"MEMORY", "performance"},
    {"TEX_ALIAS", "performance"}, {"DEPTH_INIT_INLINE", "performance"}, {"DEPTH_ALIAS_LOG", "diagnostics"}, {"TEX_DISUSE", "performance"}, {"POOL_MARK_SHARED", "performance"}, {"TEX_DISUSE_EVICT", "performance"}, {"TEX_GEN_UNDO", "performance"}, {"TEX_REUSE", "performance"}, {"TEX_SPLIT_FILL", "performance"}, {"TEX_VRAM_PRESSURE", "performance"}, {"TEX_RESIDENCY", "performance"}, {"VRAM_PAGEABLE", "performance"}, {"POOL_TEX_RELEASE", "performance"}, {"TEX_RELEASED_HOLD", "performance"}, {"TEX_PROC_COMMIT_MB", "performance"}, {"ASTC_BC", "performance"}, {"LOD_GUARD", "performance"}, {"FIX_C0", "performance"}, {"FIX_ZERO", "performance"}, {"WATER_PROJECTIVE", "performance"},
    {"SHADER_COMPRESS", "performance"}, {"SHADER_COMPRESS_IDLE_S", "performance"},
    {"BIND_SKIP", "performance"}, {"SHADER_CACHE_FLUSH", "performance"}, {"SHADER_CARRY", "performance"}, {"PIPELINE_CACHE_PLAY", "performance"}, {"PIPELINE_CACHE_PLAY_MB", "performance"}, {"PIPELINE_CACHE_STAMP", "performance"}, {"FFX_PIPE_CACHE", "performance"}, {"SHADER_MEMO", "performance"}, {"PIPE_MEMO", "performance"}, {"TEX_MEMO", "performance"}, {"DESC_TEMPLATE", "performance"},
    {"TEX_CLOBBER", "performance"}, {"LOW_END_GPU", "performance"},
    {"PUSH_SKIP", "performance"}, {"SPLIT_DRAWS", "performance"}, {"SPLIT_IDLE", "performance"}, {"SPLIT_IN_PASS", "performance"}, {"SPLIT_US", "performance"}, {"VK_VALIDATION", "performance"},
    {"VK_DYN_VERTEX", "performance"}, {"VK_DYN_A2C", "performance"}, {"VK_SUBGROUP_PIN", "performance"}, {"VK_MESH_SUBGROUPS", "performance"}, {"VK_MESH_INVARIANT", "performance"}, {"VK_RECONVERGE", "performance"},
    {"VK_NAN_PRESERVE", "performance"}, {"VK_NAN_BITS", "performance"},
    {"DRAW_CRUMBS", "diagnostics"}, {"TRACE", "diagnostics"}, {"FLIGHT", "diagnostics"}, {"PASS_CRUMBS", "diagnostics"}, {"FLIGHT_SPIKE_MS", "diagnostics"}, {"STUTTER_MAP", "diagnostics"}, {"PAR_JOBS", "performance"}, {"PIPE_RELEASE", "performance"},
    {"FLIGHT_FLASH", "diagnostics"}, {"FLIGHT_FLASH_DIFF", "diagnostics"}, {"FLIGHT_FLASH_CELLS", "diagnostics"},
    {"WB_FENCE_LIMIT", "diagnostics"}, {"WB_MERGE", "diagnostics"}, {"WB_EXACT", "diagnostics"}, {"MEM_CENSUS", "diagnostics"}, {"WB_NO_REUP", "diagnostics"}, {"WB_PROBE", "diagnostics"}, {"WB_SPLIT", "performance"}, {"SHADOW_BULK_WRITE", "performance"}, {"WB_LAZY", "performance"}, {"HEAP_LAZY", "performance"}, {"BARRIER_GAME", "performance"}, {"MESH_AHEAD", "performance"}, {"MESH_FALLBACK", "performance"}, {"MESH_FALLBACK_MB", "performance"}, {"MESH_FALLBACK_WORKERS", "performance"}, {"MESH_FALLBACK_HOLD", "performance"}, {"MESH_FALLBACK_BATCH", "performance"}, {"WB_LAZY_TOUCH", "diagnostics"}, {"WB_LAZY_UNLOCKED", "diagnostics"}, {"WB_LAZY_CHUNK", "performance"}, {"FLUSH_UPLOAD", "diagnostics"}, {"GPA_TWIN_MAX_KB", "diagnostics"}, {"MIRROR_CAPTURE_KB", "diagnostics"}, {"WB_SKIP_MB", "diagnostics"},
    {"SVC_SLOW_MS", "diagnostics"}, {"REPLAY_TIME", "diagnostics"}, {"REPLAY_PHASES", "diagnostics"}, {"LOCK_TIME", "diagnostics"},
    {"SHADOW_TABLE_STORES", "diagnostics"}, {"SHADOW_CHECK", "diagnostics"}, {"PREPARE_PIPELINES", "performance"},
    {"WB_MERGE_MAX", "diagnostics"}, {"MV_SURVEY", "diagnostics"}, {"TEXUP_LOG_MS", "diagnostics"},
    {"WAIT_WATCH_S", "diagnostics"},
    {"PRESENT_ORDER", "diagnostics"},
    {"HANDLE_CENSUS", "diagnostics"},
    {"HANG_WATCH_S", "diagnostics"},
    {"VMA_BLOCK_MB", "diagnostics"}, {"DEVICE_LOST_AT", "diagnostics"}, {"STAGING_FAIL_AT", "diagnostics"}, {"VRAM_BALLAST_MB", "diagnostics"},
    {"GEN_SEED", "diagnostics"},
    {"COPY_RETIRE", "diagnostics"},
    {"MEM_BUDGET_TEST_VRAM_MB", "diagnostics"}, {"MEM_BUDGET_TEST_RAM_MB", "diagnostics"},
    {"VRAM_WARNING", "display"},
    {"TEX_COMPACT", "performance"}, {"TEX_COMPACT_MIN_MB", "performance"}, {"TEX_COMPACT_FRAME_MB", "performance"},
};

#define LIVE 1
#define RESTART 0
#ifdef _WIN32
#define WB_LAZY_DEFAULT "1"
#else
#define WB_LAZY_DEFAULT "0"
#endif

static const CfgMeta metas[] = {
    {"FULLSCREEN", "0", "0|1", LIVE}, {"WINDOW_SIZE", "1920x1080", NULL, LIVE},
    {"VK_PRESENT", "mailbox", "mailbox|fifo|immediate", LIVE}, {"GPU_INDEX", "", NULL, RESTART},
    {"GPU_NAME", "", NULL, RESTART},
    {"PRESET", "recommended", "recommended|low|medium|high|ultra", LIVE},
    {"PRESET_PROBE", "1", "0|1", RESTART},
    {"LOW_LATENCY", "0", "0|1|2", LIVE},
    {"MONITOR", "auto", NULL, RESTART},
    {"PERF_OVERLAY", "off", "off|fps|full", LIVE},
    {"OUTPUT_RES", "1920x1080", NULL, RESTART}, {"RENDER_SCALE", "game", "game|50|67|75|100|125|150|200", RESTART},
    {"UPSCALER", "fsr", "fsr|bilinear", LIVE}, {"SHARPNESS", "80", "0..100", LIVE},
    {"STANDIN_FILTER", "easu", "easu|bilinear", LIVE},
    {"SCENE_SHARE", "0.75", "0..1", LIVE},
    {"DLAA", "0", "0|1", LIVE}, {"DLSS", "dlaa", "dlaa|quality|balanced|performance|ultra_performance", LIVE},
    {"DLSS_PRESET", "default", "default|k|j", LIVE}, {"DLSS_IDLE_FREE", "1", "0|1|2", RESTART}, {"TAA", "dlss", "dlss|fsr3", LIVE},
    {"UPSCALER_IDLE_WAIT", "0", "0|1", RESTART},
    {"DLSS_RANGE", "0", "0|1", RESTART},
    {"DLSS_PREBLOOM", "0", "0|1", LIVE},
    {"FSR4", "0", "0|1", LIVE},
    {"FSR4_FG", "0", "0|1", LIVE},
    {"FRAMEGEN", "off", "off|fsr3|dlss", LIVE},
    {"FRAMEGEN_MULTIPLIER", "2", "2|3|4", LIVE},
    {"FG_MULT", "2", "2|3|4|5|6|dynamic", LIVE},
    {"REFLEX_LIMIT", "1", "0|1", LIVE},
    {"DLSS_CHAR_MASK", "65", "0|35|65|100", LIVE}, {"FXAA", "1", "0|1", LIVE},
    {"SKIN_MOTION", "1", "0|1", RESTART},
    {"AA", "game", "game|off|fxaa|smaa|taa", LIVE},
    {"GAME_ASPECT", "16:9", "16:9|window|16:10|3:2|21:9|32:9", RESTART},
    {"GAME_RES", "1440", "1080|1440|1800|2160|2880|window", RESTART},
    {"WIDE_HUD", "center", "center|edges", RESTART},
    {"WIDE_MENUS", "full", "full|center", RESTART},
    {"ASPECT", "fit", "fit|stretch", LIVE}, {"FOV", "100", "70..140", LIVE},
    {"ANISO", "16", "game|2|4|8|16", LIVE},
    {"TEXTURE_DETAIL", "auto", "auto|full|reduced|low", RESTART},
    {"COLOR_PRESET", "default", "default|vivid|clarity|custom", LIVE}, {"COLOR_BRIGHTNESS", "0", "-30..30", LIVE},
    {"COLOR_CONTRAST", "100", "70..150", LIVE}, {"COLOR_SATURATION", "100", "50..200", LIVE},
    {"COLOR_GAMMA", "1", "0.8..1.2", LIVE},
    {"HDR", "0", "0|1|native", LIVE}, {"HDR_LIVE", "1", "0|1", RESTART},
    {"HDR_PEAK_NITS", "1000", "400..10000", LIVE}, {"HDR_GAME_NITS", "203", "48..500", LIVE},
    {"HDR_UI_NITS", "203", "48..500", LIVE}, {"HDR_GAMMA", "2.2", "off|2.2|2.4", LIVE},
    {"HDR_TONEMAP", "psychov35", "vanilla|psychov35|renodrt", LIVE}, {"HDR_EXPOSURE", "100", "0..200", LIVE},
    {"HDR_MIDGRAY", "vanilla", "render|vanilla", LIVE}, {"HDR_MATCH_SLOPE", "1", "0|1", LIVE},
    {"HDR_TONE_SLOPE", "50", "0..100", LIVE}, {"HDR_HUE_CLIP", "50", "0..100", LIVE},
    {"HDR_HIGHLIGHTS", "50", "0..100", LIVE}, {"HDR_SHADOWS", "50", "0..100", LIVE}, {"HDR_CONTRAST", "50", "0..100", LIVE},
    {"HDR_SATURATION", "50", "0..100", LIVE}, {"HDR_HIGHLIGHT_SAT", "50", "0..100", LIVE},
    {"HDR_BLOWOUT", "0", "0..100", LIVE}, {"HDR_FLARE", "0", "0..100", LIVE}, {"HDR_HUE", "100", "0..100", LIVE},
    {"HDR_GRADE", "100", "0..100", LIVE},
    {"DEVICE_PROFILE", "auto", "auto|desktop|deck", RESTART},
    {"SHADOW_RES", "game", "game|256|512|768|1024|1536|2048", RESTART},
    {"SHADOW_SCALE", "1", "1..4", RESTART}, {"SHADOW_COMPARE", "1", "0|1", RESTART},
    {"SHADOW_BULK_WRITE", "0", "0|1", RESTART},
    {"SHADOW_TABLE_STORES", "0", "0|1", RESTART}, {"SHADOW_CHECK", "0", "0|1", RESTART},
    {"SVC_SLOW_MS", "0", "0..60000", RESTART},
    {"SHADOW_RANGE", "100", "50..400", RESTART}, {"CONTACT_SHADOWS", "0", "0..100", RESTART},
    {"PCSS", "0", "0..200", RESTART},
    {"SHADOW_PCF", "0", "0|1", RESTART},
    {"FPS_LIMIT", "60", "30..240|display|unlimited", LIVE}, {"FPS60", "1", "0|1", LIVE},
    {"RTSS", "1", "0|1", RESTART},
    {"REALTIME", "1", "0|1", RESTART}, {"CLOTH_REALTIME", "1", "0|1", RESTART},
    {"MODS_REBUILD", "auto", "auto|off", RESTART},
    {"HIGHFPS_FIX", "1", "0|1", RESTART},
    {"HORSE_JUMP_FIX", "1", "0|1", RESTART},
    {"GOHMA_TURN_FIX", "1", "0|1", RESTART},
    {"FROZEN_ZORA_FIX", "1", "0|1", RESTART},
    {"BATTLE_MUSIC_FIX", "1", "0|1", RESTART},
    {"VOICE_LANGUAGE_FIX", "1", "0|1", RESTART},
    {"BALLOON_ANCHOR_FIX", "1", "0|1", RESTART},
    {"WATER_STEPS", "1", "0|1", RESTART},
    {"SOUND_STEPS", "1", "0|1", RESTART},
    {"SONG_STAFF", "1", "0|1", RESTART},
    {"FLOOR_CARRY", "1", "0|1", RESTART},
    {"OCARINA_TIMING", "1", "0|1", RESTART},
    {"FLOOR_PROBE", "0", NULL, RESTART},
    {"FLOOR_SCAN", "0", NULL, RESTART},
    {"FLOOR_WATCH", "0", "0|1", RESTART},
    {"LOG_SHORT_PATHS", "1", "0|1", RESTART},
    {"LANG", "", "ja|en-US|fr|de|it|es|zh-CN|ko|nl|pt|ru|zh-TW|en-GB|fr-CA|es-419|zh-Hans|zh-Hant|pt-BR|pl", RESTART},
    {"OPERATION_MODE", "docked", "docked|handheld", LIVE},
    {"TEXT_SPEED", "normal", "normal|fast|instant", LIVE},
    {"ASSUME_FOCUS", "1", "0|1", LIVE},
    {"VOLUME", "100", "0..100", LIVE}, {"MENU_SOUND_VOLUME", "80", "0..100", LIVE}, {"AUDIO_CHANNELS", "stereo", "auto|stereo|5.1", LIVE},
    {"AUDIO_DEVICE", "default", NULL, RESTART},
    {"MICROPHONE", "1", "0|1", RESTART},
    {"AUDIO_RAMP", "1", "0|1", RESTART},
    {"AUDIO_CATCHUP", "1", "0|1", RESTART},
    {"AUDIO_STRICT", "0", "0|1", RESTART},
    {"SPRINT_BUTTON", "off", "off|b|l3", LIVE}, {"SPRINT_MODE", "toggle", "toggle|hold", LIVE},
    {"MOTION", "1", "0|1", LIVE}, {"RUMBLE_STRENGTH", "130", "0..300", LIVE}, {"RUMBLE_CURVE", "0.6", "0.2..1.5", LIVE},
    {"RUMBLE_TRIGGERS", "0", "0|1", LIVE}, {"PAD_LAYOUT", "nintendo", "auto|nintendo|xbox|playstation", LIVE},
    {"DS3", "0", "0|1", LIVE},
    {"RUMBLE_HD", "0", "0|1", LIVE},
    {"GYRO_CALIBRATION", "1", "0|1", LIVE}, {"GYRO_DEADZONE", "low", "off|low|standard|high", LIVE},
    {"GYRO_TURN_SENS", "100", "10..400", LIVE}, {"GYRO_TILT_SENS", "100", "10..400", LIVE},
    {"GYRO_TURN_INVERT", "0", "0|1", LIVE}, {"GYRO_TILT_INVERT", "0", "0|1", LIVE},
    {"PROMPTS", "nintendo", "nintendo|xbox|playstation|steamdeck|pc", RESTART},
    {"STICK_DEADZONE", "8", "0..50", LIVE}, {"STICK_SENS", "1", "0.1..3", LIVE},
    {"DEBUG_MENU", "0", "0|1", LIVE},
    {"LSTICK_DEADZONE", "", "0..50", LIVE}, {"RSTICK_DEADZONE", "", "0..50", LIVE},
    {"MOUSE_MODE", "direct", "stick|direct", LIVE},
    {"MOUSE_SENS", "1", "0..10", LIVE}, {"MENU_CURSOR", "1", "0|1", LIVE},
    {"WALK_KEY", "none", NULL, LIVE}, {"WALK_SCALE", "0.51", "0.2..0.9", LIVE},
    {"MENU_LANG", "", "en|ru|pt-BR|zh-Hans", LIVE},
    {"AMIIBO_DIR", "", NULL, LIVE},
    {"AMIIBO_KEYS", "", NULL, LIVE},
    {"AMIIBO_WRITEBACK", "0", "0|1", LIVE},
    {"AMIIBO_POPUP", "1", "0|1", LIVE},
    {"AMIIBO_DAILY_LIMIT", "1", "0|1", LIVE},
    {"DISCORD_RPC", "1", "0|1", LIVE},
    {"DISCORD_APP_ID", NX2SARIA_DISCORD_APP_ID_DEFAULT, NULL, LIVE},
    {"DISCORD_IMAGE", NX2SARIA_DISCORD_IMAGE_DEFAULT, NULL, LIVE},
    {"TRACKER_API", "0", "0|1", RESTART},
    {"TRACKER_API_PORT", "6545", "1024..65535", RESTART},
    {"ASYNC_PREEMPT", "1", "0|1|2", RESTART}, {"REPLAY_ASYNC", "0", "0|1", RESTART},
    {"REPLAY_PIPE", "0", "0..4", RESTART},
    {"REPLAY_WORKERS", "auto", "auto|0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16", RESTART},
    {"TAIL_EARLY", "1", "0|1", RESTART},
    {"REPLAY_SMALL", "0", "0..63", RESTART},
    {"FREE_CORES", "0", "0|1", RESTART}, {"TIMER_1MS", "1", "0|1", RESTART},
    {"DYN_RES", "0", "0|1", LIVE}, {"DYN_RES_TARGET", "0", "0..100", LIVE}, {"DYN_RES_STEP", "45", "8..600", LIVE},
    {"DYN_RES_MIN", "0.5", "0.33..1", LIVE},
    {"RECORDER_CORE", "0", "auto|0|1", RESTART}, {"RECORDER_PRIO", "1", "0|1", RESTART}, {"READABLE_FAST", "1", "0|1|ab", RESTART}, {"REPLAY_TIME", "0", "0|1", RESTART}, {"REPLAY_PHASES", "0", "0|1", RESTART}, {"LOCK_TIME", "0", "0|1", RESTART}, {"POOL_CREATE_BUDGET", "16", "1..4096", RESTART}, {"POOL_CREATE_MS", "6", "0..100", RESTART}, {"POOL_FINI_IDLE", "0", "0|1", RESTART}, {"FREEZE_BUDGET_US", "1000", "0..100000", RESTART}, {"POOL_LOAD_BACKLOG", "512", "0..65536", RESTART},
    {"FREEZE_QUIET_MS", "5000", "0..60000", RESTART}, {"FREEZE_LOAD_HOLD_MS", "3000", "0..30000", RESTART}, {"POOL_LOAD_MS", "50", "1..1000", RESTART},
    {"SUBMIT_BATCH", "4", "1..16", RESTART},
#ifdef _WIN32
    {"SUBMIT_THREAD", "0", "0|1", RESTART},
#else
    {"SUBMIT_THREAD", "1", "0|1", RESTART},
#endif
    {"CACHE_DIE", "1", "0|1|ab", RESTART}, {"ONE_PER_CORE", "0", "0|1|ab", RESTART},
    {"PIPE_WAIT", "default", "default|draw", RESTART}, {"PIPE_SKIP", "any", "strict|any|fullscreen", RESTART},
    {"PIPE_LINK_MS", "6", "0..100", RESTART},
    {"PIPE_LINK_DRAW_MS", "3", "1..50", RESTART},
    {"PIPE_FRAME_WAIT_MS", "50", "0..500", RESTART},
    {"PIPE_SKIP_STEADY", "1", "0|1", RESTART},
    {"DRAW_WRITERS_WAIT", "1", "0|1", RESTART},
    {"PREPARE_PIPELINES", "all", "0|mesh|all", RESTART},
    {"MESH_FALLBACK", "auto", "auto|0|1", RESTART}, {"MESH_FALLBACK_MB", "128", "16..4096", RESTART},
    {"MESH_FALLBACK_WORKERS", "auto", "auto|1|2|3|4", RESTART},
    {"MESH_FALLBACK_HOLD", "1", "0|1", RESTART},
    {"MESH_FALLBACK_BATCH", "1", "1|0|auto", RESTART},
    {"PIPE_AHEAD", "1", "0|1", RESTART},
    {"LINE_WIDTH", "1", "0|1", RESTART},
    {"POOL_SHOWN_FIRST", "1", "0|1", RESTART},
    {"POOL_DATA_FIRST", "1", "0|1", RESTART},
    {"TEX_REUSE_NAMED", "0", "0|1", RESTART},
    {"TEX_OVERWRITTEN_USED", "1", "0|1", RESTART},
    {"GPL_FEEDBACK", "1", "0|1", RESTART},
    {"GPL_MESH", "1", "0|1", RESTART},
    {"PIPE_FAST_WORKERS", "1", "0|1", RESTART},
    {"FRAME_HOLD_MS", "120", "0..500", RESTART},
    {"PACE_SMOOTH", "1", "0|1", RESTART},
    {"OQ_BATCH", "1", "0|1", RESTART},
    {"CLEAR_SMALL_CPU", "1", "0|1", RESTART},
    {"OQ_HOLD", "0", "0|1", RESTART},
    {"QUAD_INDEX", "0", "0|1", RESTART},
    {"PATCH_OCCLUSION_OFF", "0", "0|1", RESTART},
    {"GATE_WAIT_MS", "10", "0..100", RESTART},
    {"LIB_PREBUILD", "1", "0|1", RESTART}, {"LIB_PREBUILD_THREADS", "", "1..64", RESTART},
    {"LIB_WARM", "1", "0|1|force", RESTART},
    {"PREPARE_STALL_S", "300", "0..86400", RESTART},
    {"PREPARE_STALL_TEST_MS", "0", "0..3600000", RESTART},
    {"PIPE_PREBUILD", "1", "0|1", RESTART}, {"PIPE_SEED", "1", "0|1", RESTART}, {"PIPE_SEED_BOOT", "auto", "0|1|auto", RESTART},
    {"PREWARM_XLATE", "", "1..64", RESTART}, {"GPL", "", "0|1", RESTART},
    {"LIB_PREBUILD_BIG_BG", "1", "0|1", RESTART}, {"PIPE_FEEDBACK", "1", "0|1|2", RESTART},
    {"PIPELINE_CACHE_MAX_MB", "", "0..65536", RESTART},
    {"FFX_PIPE_CACHE", "1", "0|1", RESTART},
    {"PIPELINE_CACHE_PLAY", "1", "0|1", RESTART}, {"PIPELINE_CACHE_PLAY_MB", "64", "1..65536", RESTART},
    {"PIPELINE_CACHE_STAMP", "1", "0|1", RESTART},
    {"NVIDIA_CACHE_DIR", "1", "0|1", RESTART},
    {"PIPE_BINARIES", "0", "0|1", RESTART}, {"ONESHOT_WAIT_MS", "30000", "0..60000", RESTART}, {"ONESHOT_BUDGET", "1", "0|1", RESTART}, {"PIPE_BINARIES_MAX_MB", "1024", "0..65536", RESTART}, {"BIG_BUILD_FREE_MB", "8192", "0..262144", RESTART},
    {"BIG_BUILD_MAX_MB", "4096", "0..262144", RESTART},
    {"BUILD_FREE_MB", "4096", "0..1048576", RESTART}, {"BUILD_RESERVE_MB", "64", "0..65536", RESTART},
    {"BG_BUILD_PLAY_DRAWS", "600", "0..1000000", RESTART},
    {"MEM_NEED_MB", "24576", "0..1048576", RESTART},
    {"COMPUTE_KEEP", "512", NULL, RESTART},
    {"SHADER_CARRY", "1", "0|1", RESTART},
    {"SHADER_COMPRESS", "1", "0|1", RESTART},
    {"SHADER_COMPRESS_IDLE_S", "30", "1..3600", RESTART},
    {"TEX_RELEASED_MAX", "", NULL, RESTART},
    {"MIRROR_HOT", "0", "0..1000", RESTART}, {"FREEZE_IDLE", "1800", "0..100000", RESTART},
    {"SUBMIT_FINISH_KINDS", "31", "0..31", RESTART},
    {"FOLIAGE", "full", "full|reduced|off", RESTART},
    {"VRAM_HEAP_LIMIT", "0", NULL, RESTART},
    {"VRAM_RESERVE_MB", "0", "0..4096", RESTART},
    {"UPLOAD_BATCH_MB", "0", NULL, RESTART},
    {"MESH_EMULATION", "0", "0|1", RESTART}, {"MESH_EMU_MB", "256", "32..2048", RESTART},
    {"MESH_EMU_TASKS", "16384", "256..262144", RESTART}, {"MESH_EMU_BATCH", "1", "0|1", RESTART}, {"VEC_INDEX_FIX", "0", "0|auto|1", RESTART},
    {"GUEST_OPT", "1", "1|2", RESTART},
    {"PASS_TIMES", "0", "0|1", RESTART}, {"DUMP_KEY", "", NULL, RESTART},
    {"ASTC", "auto", "auto|gpu|native|cpu", RESTART}, {"MEMORY", "auto", "auto|import|mirror|verify", RESTART},
    {"TEX_ALIAS", "1", "0|1", RESTART}, {"DEPTH_INIT_INLINE", "1", "0|1", RESTART}, {"DEPTH_ALIAS_LOG", "0", "0|1", RESTART}, {"TEX_DISUSE", "1", "0|1|2", RESTART}, {"POOL_MARK_SHARED", "1", "0|1", RESTART}, {"TEX_DISUSE_EVICT", "0", "0|1", RESTART}, {"TEX_GEN_UNDO", "1", "0|1", RESTART}, {"TEX_REUSE", "1", "0|1", RESTART}, {"TEX_SPLIT_FILL", "1", "0|1|2", RESTART}, {"TEX_VRAM_PRESSURE", "80", "0..100", RESTART},
    {"TEX_RELEASED_HOLD", "60", "0..900", RESTART}, {"ASTC_BC", "", "0|1", RESTART}, {"ZSTD_NATIVE", "1", "0|1", RESTART}, {"ZSTD_CHECK", "0", "0|1", RESTART},
    {"TEX_CLOBBER", "0", "0|1", RESTART},
    {"LOW_END_GPU", "0", "0|1", RESTART},
    {"STG_ARENA", "1", "0|1", RESTART}, {"ASTC_GPU_DESWIZZLE", "1", "0|1", RESTART},
    {"ASTC_SCRATCH_KEEP", "1", "0|1", RESTART}, {"ASTC_ARENA", "1", "0|1", RESTART},
    {"LOD_GUARD", "1", "0|1", RESTART}, {"FIX_C0", "1", "0|1", RESTART}, {"FIX_ZERO", "1", "0|1", RESTART},
    {"WATER_PROJECTIVE", "1", "0|1", RESTART},
    {"BIND_SKIP", "1", "0|1", RESTART}, {"SHADER_CACHE_FLUSH", "0", "0|1", RESTART}, {"SHADER_MEMO", "1", "0|1", RESTART}, {"PIPE_MEMO", "1", "0|1", RESTART}, {"TEX_MEMO", "1", "0|1", RESTART},
    {"BIND_REUSE", "1", "0|1", RESTART}, {"DYN_DELTA", "1", "0|1", RESTART},
    {"DESC_TEMPLATE", "1", "0|1", RESTART},
    {"PUSH_SKIP", "1", "0|1", RESTART}, {"SPLIT_DRAWS", "64", "0..1000", RESTART},
    {"SPLIT_US", "1000", "0..20000", RESTART}, {"SPLIT_IDLE", "0", "0|1", RESTART}, {"SPLIT_IN_PASS", "1", "0|1", RESTART},
    {"VK_VALIDATION", "0", "0|1", RESTART}, {"VK_DYN_VERTEX", "", "0|1", RESTART},
    {"VK_DYN_A2C", "", "0|1", RESTART}, {"VK_SUBGROUP_PIN", "1", "0|1", RESTART},
    {"VK_RECONVERGE", "1", "0|1", RESTART},
    {"VK_MESH_SUBGROUPS", "1", "0|1", RESTART},
    {"VK_MESH_INVARIANT", "1", "0|1", RESTART},
    {"VK_NAN_PRESERVE", "", "0|1", RESTART},
    {"VK_NAN_BITS", "", "0|1", RESTART},
    {"VK_NOOPT", "", "0|1|mesh", RESTART},
    {"TRACE", "", NULL, RESTART}, {"FLIGHT", "0", "0|1", RESTART}, {"FLIGHT_SPIKE_MS", "100", "0..60000", RESTART},
    {"STUTTER_MAP", "0", "0|1", RESTART},
    {"PAR_JOBS", "1", "0|1", RESTART},
    {"PIPE_RELEASE", "1", "0|1", RESTART},
    {"FLIGHT_FLASH", "1", "0|1", RESTART}, {"FLIGHT_FLASH_DIFF", "24", "4..255", RESTART}, {"FLIGHT_FLASH_CELLS", "3", "1..144", RESTART},
    {"WB_FENCE_LIMIT", "65536", "0..1073741824", RESTART}, {"WB_MERGE", "1", "0|1", RESTART}, {"WB_EXACT", "1", "0|1", RESTART}, {"MEM_CENSUS", "0", "0|1", RESTART}, {"WB_NO_REUP", "1", "0|1", RESTART}, {"WB_PROBE", "0", "0|1", RESTART}, {"WB_SPLIT", "1", "0|1|2|3", RESTART}, {"WB_LAZY", WB_LAZY_DEFAULT, "0|1", RESTART}, {"HEAP_LAZY", "0", "0|1", RESTART}, {"WB_LAZY_TOUCH", "0", "0..16", RESTART}, {"WB_LAZY_CHUNK", "0", "0|1", RESTART}, {"WB_LAZY_UNLOCKED", "1", "0|1", RESTART}, {"FLUSH_UPLOAD", "0", "0|1", RESTART}, {"GPA_TWIN_MAX_KB", "0", "0..1048576", RESTART},
    {"MIRROR_CAPTURE_KB", "0", "0..1048576", RESTART}, {"WB_SKIP_MB", "0", "0..4096", RESTART},
    {"WB_MERGE_MAX", "32", "1..256", RESTART}, {"TEXUP_LOG_MS", "0", "0..60000", RESTART},
    {"WAIT_WATCH_S", "0", "0..3600", RESTART},
    {"PRESENT_ORDER", "1", "0|1", RESTART},
    {"HANDLE_CENSUS", "0", "0|1", RESTART},
    {"HANG_WATCH_S", "20", "0..600", RESTART},
    {"VMA_BLOCK_MB", "64", "16..256", RESTART},
    {"TEX_COMPACT", "0", "0|1", RESTART}, {"TEX_COMPACT_MIN_MB", "256", "64..65536", RESTART},
    {"TEX_COMPACT_FRAME_MB", "8", "1..32", RESTART},
    {"ASTC_BC1", "1", "0|1", RESTART},
    {"VRAM_PAGEABLE", "1", "0|1", RESTART},
    {"TEX_RESIDENCY", "1", "0|1", RESTART},
    {"POOL_TEX_RELEASE", "1", "0|1", RESTART},
    {"PASS_CRUMBS", "", "0|1", RESTART},
    {"DRAW_CRUMBS", "0", "0|1", RESTART},
    {"STAGING_FAIL_AT", "0", "0..1000000000", RESTART},
    {"VRAM_BALLAST_MB", "0", "0..131072", RESTART},
    {"GEN_SEED", "0", "0..4611686018427387904", RESTART},
    {"MEM_BUDGET_TEST_VRAM_MB", "0", "0..1048576", RESTART},
    {"MEM_BUDGET_TEST_RAM_MB", "0", "0..1048576", RESTART},
    {"VRAM_WARNING", "1", "0|1", RESTART},
    {"DEVICE_LOST_AT", "0", "0..1000000000", RESTART},
    {"COPY_RETIRE", "0", "0|1", RESTART},
    {"INPUT_RECORD", "", NULL, RESTART},
};

static int is_known(const char *key) {
    for (size_t i = 0; i < sizeof known / sizeof *known; i++)
        if (!strcmp(known[i], key)) return 1;
    return !strncmp(key, "CHEAT_", 6);
}

static char unknown_keys[384];
static int n_unknown, unknown_cut;

static void note_unknown(const char *key) {
    char low[64];
    size_t n = 0;
    for (; key[n] && n < sizeof low - 1; n++) {
        if (!isalnum((unsigned char)key[n]) && key[n] != '_') break;
        low[n] = (char)tolower((unsigned char)key[n]);
    }
    if (key[n]) {
        n_unknown++, unknown_cut = 1;
        return;
    }
    low[n] = 0;
    size_t have = strlen(unknown_keys);
    for (const char *p = unknown_keys; (p = strstr(p, low)) != NULL; p += n)
        if ((p == unknown_keys || p[-1] == ' ') && (p[n] == ',' || !p[n])) return;
    n_unknown++;
    if (have + n + 3 > sizeof unknown_keys) {
        unknown_cut = 1;
        return;
    }
    snprintf(unknown_keys + have, sizeof unknown_keys - have, "%s%s", have ? ", " : "", low);
}

const char *cfg_unknown_keys(int *count) {
    if (count) *count = n_unknown;
    return unknown_keys;
}

static int full_load_told, full_set_told;

static void full_set(const char *key, const char *what) {
    if (!full_set_told++) rt_log("cfg: the settings table is full (%d keys): %s %s", CFG_MAX_KEYS, key, what);
}

static void clear(void) {
    for (int i = 0; i < n_entries; i++) free(entries[i].key), free(entries[i].value);
    n_entries = 0;
    unknown_keys[0] = 0, n_unknown = unknown_cut = 0;
    full_load_told = 0;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static char *norm_key(char *key) {
    for (char *p = key; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (!strncmp(key, PREFIX, PREFIX_LEN)) return key + PREFIX_LEN;
    return strncmp(key, OLD_PREFIX, OLD_PREFIX_LEN) ? key : key + OLD_PREFIX_LEN;
}

static int entry_line[CFG_MAX_KEYS];

#define CHECK_LINES 16
static int cuts;

static char *cut_comment(const char *key, char *raw, int line) {
    char *v = trim(raw);
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') return v;
    const CfgMeta *m = cfg_meta(key);
    if (!m || !m->values) return v;
    char *c = raw;
    while (*c && !((*c == ' ' || *c == '\t') && (c[1] == ';' || c[1] == '#'))) c++;
    if (!*c) return v;
    char note[160];
    snprintf(note, sizeof note, "%s", c + 1);
    *c = 0;
    v = trim(raw);
    if (++cuts <= CHECK_LINES) {
        char low[64];
        size_t k = 0;
        for (; key[k] && k < sizeof low - 1; k++) low[k] = (char)tolower((unsigned char)key[k]);
        low[k] = 0;
        rt_log("cfg: settings file line %d: %s = '%s', and '%s' after it is a comment", line, low, v, note);
    }
    return v;
}

#define OLDER_MAX 32
static PlatMutex older_lock = PLAT_MUTEX_INIT;
static char older_told[OLDER_MAX][72];
static int n_older_told, older_full;

static int older_first(char where, const char *key) {
    char k[72];
    snprintf(k, sizeof k, "%c:%s", where, key);
    for (char *p = k + 2; *p; p++) *p = (char)toupper((unsigned char)*p);
    int first = 0;
    plat_mutex_lock(&older_lock);
    int i = 0;
    while (i < n_older_told && strcmp(older_told[i], k)) i++;
    if (i == n_older_told) {
        if (n_older_told < OLDER_MAX) memcpy(older_told[n_older_told++], k, sizeof k), first = 1;
        else if (!older_full) older_full = 1, first = -1;
    }
    plat_mutex_unlock(&older_lock);
    if (first < 0) rt_log("cfg: more than %d settings use their older NX2NAVI_ names; the rest are not listed", OLDER_MAX);
    return first > 0;
}

static void older_file_key(const char *raw, int line) {
    if (strncasecmp(raw, OLD_PREFIX, OLD_PREFIX_LEN)) return;
    const char *key = raw + OLD_PREFIX_LEN;
    char up[64], low[64];
    size_t n = 0;
    for (; key[n] && n < sizeof up - 1; n++)
        up[n] = (char)toupper((unsigned char)key[n]), low[n] = (char)tolower((unsigned char)key[n]);
    up[n] = low[n] = 0;
    if (!*up || !is_known(up) || !older_first('f', up)) return;
    rt_log("cfg: settings file line %d: %s read under its older name; %s is the current one", line, raw, low);
}

#ifdef _WIN32
#define CFG_ENVIRON _environ
#else
extern char **environ;
#define CFG_ENVIRON environ
#endif
static void older_env(void) {
    for (char **e = CFG_ENVIRON; e && *e; e++) {
        if (strncasecmp(*e, OLD_PREFIX, OLD_PREFIX_LEN)) continue;
        const char *key = *e + OLD_PREFIX_LEN, *eq = strchr(key, '=');
        if (!eq || eq == key || eq - key >= 64) continue;
        char up[64], legacy[96], current[96];
        size_t n = (size_t)(eq - key);
        for (size_t i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)key[i]);
        up[n] = 0;
        snprintf(legacy, sizeof legacy, OLD_PREFIX "%s", up);
        snprintf(current, sizeof current, PREFIX "%s", up);
        if (!getenv(legacy) || !is_known(up) || !older_first('e', up)) continue;
        if (getenv(current)) rt_log("cfg: %s is not read: %s is set too and wins", legacy, current);
        else rt_log("cfg: %s read under its older name; %s is the current one", legacy, current);
    }
}

static void add(char *key, char *value, int line) {
    older_file_key(key, line);
    key = norm_key(key);
    value = cut_comment(key, value, line);
    size_t vl = strlen(value);
    if (vl >= 2 && value[0] == '"' && value[vl - 1] == '"') value[vl - 1] = 0, value++;
    if (!*key) return;
    if (!is_known(key)) {
        rt_log("cfg: settings file line %d: unknown key '%s'", line, key);
        note_unknown(key);
    }
    if (!*value) return;
    for (int i = 0; i < n_entries; i++)
        if (!strcmp(entries[i].key, key)) {
            free(entries[i].value);
            entries[i].value = strdup(value);
            entry_line[i] = line;
            return;
        }
    if (n_entries == CFG_MAX_KEYS) {
        if (!full_load_told++)
            rt_log("cfg: settings file has more than %d keys: line %d (%s) and the new keys after it are not read",
                   CFG_MAX_KEYS, line, key);
        return;
    }
    entry_line[n_entries] = line;
    entries[n_entries++] = (CfgEntry){strdup(key), strdup(value), 0, 0, 0, 0};
}

static int misfits;
static int switch_of(const char *v);

static int range_fit(const char *m, size_t n, const char *v) {
    char s[64], *end;
    if (n >= sizeof s) return -1;
    memcpy(s, m, n), s[n] = 0;
    char *dots = strstr(s, "..");
    if (!dots || dots == s) return -1;
    *dots = 0;
    double lo = strtod(s, &end);
    if (*end) return -1;
    double hi = strtod(dots + 2, &end);
    if (end == dots + 2 || *end) return -1;
    double x = strtod(v, &end);
    return end != v && !*end && x == x && x >= lo && x <= hi;
}

static const char *misfit(const char *set, const char *v, char *buf, size_t cap) {
    if (!strcmp(set, "0|1")) return switch_of(v) < 0 ? "is not 0, 1, on, off, true, false, yes or no" : NULL;
    if (strstr(set, "..") && !strchr(set, '|')) {
        int fit = range_fit(set, strlen(set), v);
        if (fit) return NULL;
        snprintf(buf, cap, "is not a number in %s", set);
        return buf;
    }
    for (const char *m = set; *m;) {
        char *end;
        size_t n = strcspn(m, "|");
        strtod(m, &end);
        if (end == m + n) return NULL;
        m += n + (m[n] == '|');
    }
    for (const char *m = set; *m;) {
        size_t n = strcspn(m, "|");
        if (strlen(v) == n && !strncasecmp(m, v, n)) return NULL;
        if (range_fit(m, n, v) == 1) return NULL;
        m += n + (m[n] == '|');
    }
    snprintf(buf, cap, "is none of the menu's choices %s", set);
    return buf;
}

static void check_value(const char *key, const char *v, int line) {
    const CfgMeta *m = cfg_meta(key);
    if (!m) return;
    char buf[160];
    const char *bad = m->values ? misfit(m->values, v, buf, sizeof buf) : NULL;
    int comment = strstr(v, " ;") || strstr(v, " #") || strstr(v, "\t;") || strstr(v, "\t#");
    if (!bad && !comment) return;
    if (++misfits > CHECK_LINES) return;
    char low[64], where[96];
    size_t n = 0;
    for (; key[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)key[n]);
    low[n] = 0;
    if (line) snprintf(where, sizeof where, "settings file line %d: %s", line, low);
    else {
        char current[128], legacy[128];
        snprintf(current, sizeof current, PREFIX "%s", key);
        snprintf(legacy, sizeof legacy, OLD_PREFIX "%s", key);
        snprintf(where, sizeof where, "the environment's %s", getenv(current) ? current : legacy);
    }
    rt_log("cfg: %s = '%s' %s%s%s", where, v, bad ? bad : "", bad && comment ? "; " : "",
           comment ? "a ';' or '#' after the value is part of it (a comment goes on a line of its own)" : "");
}

static void check_end(void) {
    if (misfits > CHECK_LINES)
        rt_log("cfg: %d more values outside their settings' choices are not listed", misfits - CHECK_LINES);
    if (cuts > CHECK_LINES) rt_log("cfg: %d more comments after a value are not listed", cuts - CHECK_LINES);
    misfits = cuts = 0;
}

static void check_values(void) {
    for (int i = 0; i < n_entries; i++)
        if (entries[i].value && entry_line[i]) check_value(entries[i].key, entries[i].value, entry_line[i]);
    check_end();
}

static void migrate(void) {
    CfgEntry *layout = NULL, *prompts = NULL;
    for (int i = 0; i < n_entries; i++) {
        if (!strcmp(entries[i].key, "PAD_LAYOUT")) layout = &entries[i];
        if (!strcmp(entries[i].key, "PROMPTS")) prompts = &entries[i];
    }
    if (!layout || strcasecmp(layout->value, "xbox_prompts")) return;
    free(layout->value);
    layout->value = strdup("nintendo");
    layout->dirty = 1;
    if (!prompts && n_entries < CFG_MAX_KEYS)
        entry_line[n_entries] = 0, entries[n_entries++] = (CfgEntry){strdup("PROMPTS"), strdup("xbox"), 1, 0, 0, 0};
    else if (!prompts)
        full_set("PROMPTS", "= xbox is not kept");
    rt_log("cfg: settings file pad_layout = xbox_prompts is read as pad_layout = nintendo and prompts = %s",
           prompts ? prompts->value : "xbox");
}

static uint8_t *utf16_text(const uint8_t *d, uint64_t size) {
    if (size < 2 || !((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) return NULL;
    int be = d[0] == 0xFE;
    uint8_t *out = malloc((size_t)(size / 2) * 3 + 1), *o = out;
    if (!out) return NULL;
    for (uint64_t i = 2; i + 1 < size; i += 2) {
        uint32_t c = be ? (uint32_t)d[i] << 8 | d[i + 1] : (uint32_t)d[i + 1] << 8 | d[i];
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < size) {
            uint32_t lo = be ? (uint32_t)d[i + 2] << 8 | d[i + 3] : (uint32_t)d[i + 3] << 8 | d[i + 2];
            if (lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), i += 2;
        }
        if (c < 0x80) *o++ = (uint8_t)c;
        else if (c < 0x800) *o++ = (uint8_t)(0xC0 | c >> 6), *o++ = (uint8_t)(0x80 | (c & 0x3F));
        else if (c < 0x10000)
            *o++ = (uint8_t)(0xE0 | c >> 12), *o++ = (uint8_t)(0x80 | (c >> 6 & 0x3F)), *o++ = (uint8_t)(0x80 | (c & 0x3F));
        else
            *o++ = (uint8_t)(0xF0 | c >> 18), *o++ = (uint8_t)(0x80 | (c >> 12 & 0x3F)),
            *o++ = (uint8_t)(0x80 | (c >> 6 & 0x3F)), *o++ = (uint8_t)(0x80 | (c & 0x3F));
    }
    *o = 0;
    return out;
}

static int parse_ini(const char *path, void (*sink)(char *key, char *value, int line, void *ctx), void *ctx) {
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    if (!data) return 0;
    uint8_t *wide = utf16_text(data, size);
    if (wide) {
        free(data);
        data = wide, size = strlen((const char *)wide);
        rt_log("cfg: %s is saved as UTF-16; read as that (the menu saves it as UTF-8)", path);
    }
    char *p = (char *)data;
    if (size >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
    for (int line = 1; *p; line++) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        char *s = trim(p);
        char *eq = strchr(s, '=');
        if (*s && *s != '#' && *s != ';' && *s != '[') {
            if (!eq) rt_log("cfg: settings file line %d: no '=' in '%s'", line, s);
            else {
                *eq = 0;
                sink(trim(s), eq + 1, line, ctx);
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(data);
    return 1;
}

static void add_sink(char *key, char *value, int line, void *ctx) {
    (void)ctx;
    add(key, value, line);
}

int cfg_load(const char *path) {
    clear();
    __atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);
    if (!parse_ini(path, add_sink, NULL)) return 0;
    migrate();
    check_values();
    return 1;
}

static const struct { const char *key, *value; } DECK[] = {
    {"FULLSCREEN", "1"}, {"WINDOW_SIZE", "1280x800"}, {"GAME_ASPECT", "window"}, {"RENDER_SCALE", "game"},
    {"VK_PRESENT", "fifo"}, {"FPS_LIMIT", "40"}, {"CORES", "4"}, {"ASTC_BC", "1"}, {"SPIN_HANDOFF_US", "30"},
#ifdef CFG_TEST_DECK_ROW
    CFG_TEST_DECK_ROW,
#endif
};
static int profile = -1;

static int read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    out[ok ? strcspn(out, "\r\n") : 0] = 0;
    return ok;
}

static int on_steam_deck(void) {
    const char *env = getenv("SteamDeck");
    if (env && !strcmp(env, "1")) return 1;
    char vendor[64], product[64];
    return read_line("/sys/class/dmi/id/board_vendor", vendor, sizeof vendor) && !strcmp(vendor, "Valve") &&
           read_line("/sys/class/dmi/id/product_name", product, sizeof product) &&
           (!strcmp(product, "Jupiter") || !strcmp(product, "Galileo"));
}

static const char *raw_get(const char *name);

static int lowend_set(void) {
    const char *set = raw_get(PREFIX "LOW_END_GPU");
    return set && *set && switch_of(set) != 0;
}

static int profile_of(void) {
    int p = __atomic_load_n(&profile, __ATOMIC_ACQUIRE);
    if (p >= 0) return p;
    const char *set = raw_get(PREFIX "DEVICE_PROFILE");
    if (set && !strcmp(set, "deck")) p = 1;
    else if (set && *set && strcmp(set, "auto")) p = 0;
    else p = on_steam_deck();
    __atomic_store_n(&profile, p, __ATOMIC_RELEASE);
    return p;
}

const char *cfg_device_profile(void) { return profile_of() ? "deck" : "desktop"; }

static PresetHw preset_hw;
static int preset_tier = -1;

static const char *preset_layer(const char *key) {
    int t = __atomic_load_n(&preset_tier, __ATOMIC_ACQUIRE);
    return t >= 0 ? preset_value(t, &preset_hw, key) : NULL;
}

const PresetHw *cfg_preset_hw(void) { return __atomic_load_n(&preset_tier, __ATOMIC_ACQUIRE) >= 0 ? &preset_hw : NULL; }

const char *cfg_env(const char *name) {
    const char *key = name;
    if (!strncmp(key, PREFIX, PREFIX_LEN)) key += PREFIX_LEN;
    else if (!strncmp(key, OLD_PREFIX, OLD_PREFIX_LEN)) key += OLD_PREFIX_LEN;
    else return getenv(name);
    char current[128], legacy[128];
    if (snprintf(current, sizeof current, PREFIX "%s", key) >= (int)sizeof current ||
        snprintf(legacy, sizeof legacy, OLD_PREFIX "%s", key) >= (int)sizeof legacy) return NULL;
    const char *e = getenv(current);
    return e ? e : getenv(legacy);
}

static const char *raw_get(const char *name) {
    const char *e = cfg_env(name);
    if (strncmp(name, OLD_PREFIX, OLD_PREFIX_LEN) && strncmp(name, PREFIX, PREFIX_LEN)) return e;
    int n = __atomic_load_n(&n_entries, __ATOMIC_ACQUIRE);
    const char *key = !strncmp(name, PREFIX, PREFIX_LEN) ? name + PREFIX_LEN : name + OLD_PREFIX_LEN;
    if (e) return e;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(entries[i].key, key)) return __atomic_load_n(&entries[i].value, __ATOMIC_ACQUIRE);
    return NULL;
}

static int switch_of(const char *v) {
    static const char *const word[] = {"false", "off", "no", "true", "on", "yes"};
    if (!strcmp(v, "0") || !strcmp(v, "1")) return v[0] - '0';
    for (int i = 0; i < 6; i++)
        if (!strcasecmp(v, word[i])) return i >= 3;
    return -1;
}

static const char *switch_word(const char *name, const char *v) {
    if (!v || !strcmp(v, "0") || !strcmp(v, "1")) return v;
    int on = switch_of(v);
    if (on < 0) return v;
    const CfgMeta *m = cfg_meta(name);
    return m && m->values && !strcmp(m->values, "0|1") ? (on ? "1" : "0") : v;
}

static const char *cfg_get_now(const char *name) {
    const char *v = raw_get(name);
    if (strncmp(name, OLD_PREFIX, OLD_PREFIX_LEN) && strncmp(name, PREFIX, PREFIX_LEN)) return v;
    if (v) return switch_word(name, v);
    const char *key = !strncmp(name, PREFIX, PREFIX_LEN) ? name + PREFIX_LEN : name + OLD_PREFIX_LEN;
    if (profile_of())
        for (size_t i = 0; i < sizeof DECK / sizeof *DECK; i++)
            if (!strcasecmp(DECK[i].key, key)) return DECK[i].value;
    return preset_layer(key);
}

int cfg_is_set(const char *name) { return raw_get(name) != NULL; }

unsigned cfg_version(void) { return __atomic_load_n(&version, __ATOMIC_ACQUIRE); }

int cfg_setenv(const char *name, const char *value) {
    const char *key = !strncmp(name, PREFIX, PREFIX_LEN) ? name + PREFIX_LEN :
                      !strncmp(name, OLD_PREFIX, OLD_PREFIX_LEN) ? name + OLD_PREFIX_LEN : NULL;
    char current[128];
    if (key && snprintf(current, sizeof current, PREFIX "%s", key) < (int)sizeof current) name = current;
    int ok = plat_setenv(name, value);
    __atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);
    return ok;
}

int cfg_from_env(const char *name) { return cfg_env(name) != NULL; }

static void set_value(const char *name, const char *value, int test) {
    if ((strncmp(name, OLD_PREFIX, OLD_PREFIX_LEN) && strncmp(name, PREFIX, PREFIX_LEN)) || !value) return;
    char key[64];
    snprintf(key, sizeof key, "%s", !strncmp(name, PREFIX, PREFIX_LEN) ? name + PREFIX_LEN : name + OLD_PREFIX_LEN);
    for (char *p = key; *p; p++) *p = (char)toupper((unsigned char)*p);
    plat_mutex_lock(&set_lock);
    int i = 0, n = n_entries;
    while (i < n && strcmp(entries[i].key, key)) i++;
    if (i < n) {
        const int changed = !entries[i].value || strcmp(entries[i].value, value);
        if (changed) {
            if (entries[i].value) kept_n++, kept_bytes += strlen(entries[i].value) + 1;
            __atomic_store_n(&entries[i].value, strdup(value), __ATOMIC_RELEASE);
        }
        if (test && !changed && entries[i].dirty && !entries[i].test) test = 0;
        if (changed) entries[i].dirty = 1;
        entries[i].removed = 0;
        entries[i].test = test;
    } else if (n < CFG_MAX_KEYS) {
        entries[n] = (CfgEntry){strdup(key), strdup(value), 1, 0, 0, test};
        __atomic_store_n(&n_entries, n + 1, __ATOMIC_RELEASE);
    } else {
        full_set(key, "is not kept");
    }
    __atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&set_lock);
}

void cfg_set(const char *name, const char *value) { set_value(name, value, 0); }

int cfg_reset(const char *const *keys, int n) {
    int done = 0;
    plat_mutex_lock(&set_lock);
    for (int k = 0; k < n; k++) {
        char key[64], var[80];
        snprintf(key, sizeof key, "%s", keys[k]);
        const char *name = norm_key(key);
        snprintf(var, sizeof var, PREFIX "%s", name);
        if (cfg_env(var)) continue;
        int i = 0;
        while (i < n_entries && strcmp(entries[i].key, name)) i++;
        if (i == n_entries) {
            if (i == CFG_MAX_KEYS) {
                full_set(name, "keeps its line in the file");
                continue;
            }
            entries[i] = (CfgEntry){strdup(name), NULL, 0, 1, 0, 0};
            __atomic_store_n(&n_entries, i + 1, __ATOMIC_RELEASE);
        } else if (entries[i].value) {
            __atomic_store_n(&entries[i].value, NULL, __ATOMIC_RELEASE);
            entries[i].dirty = 0;
            entries[i].removed = 1;
            entries[i].test = 0;
            done++;
        } else {
            entries[i].removed = 1;
            entries[i].test = 0;
        }
    }
    if (done) __atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&set_lock);
    return done;
}

static int find_entry(const char *key) {
    for (int i = 0; i < n_entries; i++)
        if (!strcmp(entries[i].key, key)) return i;
    return -1;
}

static void entry_key(const char *name, char *key, size_t size) {
    const char *suffix = !strncasecmp(name, PREFIX, PREFIX_LEN) ? name + PREFIX_LEN :
                         !strncasecmp(name, OLD_PREFIX, OLD_PREFIX_LEN) ? name + OLD_PREFIX_LEN : name;
    snprintf(key, size, "%s", suffix);
    for (char *p = key; *p; p++) *p = (char)toupper((unsigned char)*p);
}

int cfg_state(const char *name, const char **value) {
    char key[64];
    entry_key(name, key, sizeof key);
    plat_mutex_lock(&set_lock);
    int i = find_entry(key), state = 0;
    *value = NULL;
    if (i >= 0) {
        *value = entries[i].value;
        state = (entries[i].dirty ? CFG_DIRTY : 0) | (entries[i].removed ? CFG_REMOVED : 0) | (entries[i].test ? CFG_TEST : 0);
    }
    plat_mutex_unlock(&set_lock);
    return state;
}

void cfg_hold(const char *name, int on) {
    char key[64];
    entry_key(name, key, sizeof key);
    plat_mutex_lock(&set_lock);
    int i = find_entry(key);
    if (i < 0 && on && n_entries < CFG_MAX_KEYS) {
        i = n_entries;
        entries[i] = (CfgEntry){strdup(key), NULL, 0, 0, 0, 0};
        __atomic_store_n(&n_entries, i + 1, __ATOMIC_RELEASE);
    }
    if (i >= 0) entries[i].held = on != 0;
    plat_mutex_unlock(&set_lock);
}

void cfg_set_test(const char *name, const char *value) { set_value(name, value, 1); }

int cfg_test_keep(void) {
    int n = 0;
    plat_mutex_lock(&set_lock);
    for (int i = 0; i < n_entries; i++)
        if (entries[i].test) entries[i].test = 0, n++;
    plat_mutex_unlock(&set_lock);
    return n;
}

void cfg_revert(const char *name, const char *orig_value, int orig_state) {
    char key[64];
    entry_key(name, key, sizeof key);
    plat_mutex_lock(&set_lock);
    int i = find_entry(key);
    if (i >= 0) {
        __atomic_store_n(&entries[i].value, (char *)orig_value, __ATOMIC_RELEASE);
        entries[i].dirty = (orig_state & CFG_DIRTY) != 0;
        entries[i].removed = (orig_state & CFG_REMOVED) != 0;
        entries[i].test = (orig_state & CFG_TEST) != 0;
        entries[i].held = 0;
        __atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&set_lock);
}

int cfg_last_save_ok(void) { return __atomic_load_n(&save_ok, __ATOMIC_ACQUIRE); }

void cfg_each_active(void (*cb)(const char *key, const char *value, int from_env, void *ctx), void *ctx) {
    int n = __atomic_load_n(&n_entries, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        const char *v = __atomic_load_n(&entries[i].value, __ATOMIC_ACQUIRE);
        if (v) cb(entries[i].key, v, 0, ctx);
    }
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) {
        char var[80];
        snprintf(var, sizeof var, PREFIX "%s", known[i]);
        const char *e = cfg_env(var);
        if (e) cb(known[i], e, 1, ctx);
    }
}

const CfgMeta *cfg_meta(const char *key) {
    char k[64];
    snprintf(k, sizeof k, "%s", key);
    const char *name = norm_key(k);
    for (size_t i = 0; i < sizeof metas / sizeof *metas; i++)
        if (!strcmp(metas[i].key, name)) return &metas[i];
    return NULL;
}

const char *cfg_default(const char *key) {
    char k[64];
    snprintf(k, sizeof k, "%s", key);
    const char *name = norm_key(k);
    if (profile_of())
        for (size_t i = 0; i < sizeof DECK / sizeof *DECK; i++)
            if (!strcasecmp(DECK[i].key, name)) return DECK[i].value;
    const char *pv = preset_layer(name);
    if (pv) return pv;
    const CfgMeta *m = cfg_meta(key);
    return m ? m->def : NULL;
}

const CfgMeta *cfg_meta_at(int i) { return i >= 0 && (size_t)i < sizeof metas / sizeof *metas ? &metas[i] : NULL; }

static const char *cfg_value_now(const char *name) {
    const char *e = cfg_get_now(name);
    if (e && *e) return e;
    const CfgMeta *m = cfg_meta(name);
    if (!m) rt_fatal("cfg: %s has no row in the settings table", name);
    return m->def;
}

#define CFG_MEMO_N 256
typedef struct { unsigned ver; uint32_t h; uint8_t kind; char name[55]; const char *v; } CfgMemo;
static _Thread_local CfgMemo cfg_memo[CFG_MEMO_N];
static int cfg_memo_on = -1;

static const char *cfg_memo_get(const char *name, int kind) {
    if (cfg_memo_on < 0) {
        const char *e = cfg_env(PREFIX "CFG_MEMO");
        cfg_memo_on = !(e && !strcmp(e, "0"));
    }
    size_t len = strlen(name);
    if (!cfg_memo_on || len >= sizeof cfg_memo[0].name) return kind ? cfg_value_now(name) : cfg_get_now(name);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)name[i]) * 16777619u;
    h = h * 2 + (uint32_t)kind;
    CfgMemo *m = &cfg_memo[h % CFG_MEMO_N];
    unsigned ver = __atomic_load_n(&version, __ATOMIC_ACQUIRE) + 1;
    if (m->ver == ver && m->h == h && m->kind == kind && !memcmp(m->name, name, len + 1)) return m->v;
    const char *v = kind ? cfg_value_now(name) : cfg_get_now(name);
    m->ver = ver, m->h = h, m->kind = (uint8_t)kind, m->v = v;
    memcpy(m->name, name, len + 1);
    return v;
}

const char *cfg_get(const char *name) { return cfg_memo_get(name, 0); }
const char *cfg_value(const char *name) { return cfg_memo_get(name, 1); }

static double default_number(const char *name) {
    const CfgMeta *m = cfg_meta(name);
    return m ? strtod(m->def, NULL) : 0;
}

double cfg_double(const char *name) {
    const char *v = cfg_value(name);
    char *end;
    double d = strtod(v, &end);
    return end == v || *end || d != d ? default_number(name) : d;
}

int cfg_int(const char *name) {
    const char *v = cfg_value(name);
    char *end;
    long l = strtol(v, &end, 10);
    return end == v || *end ? (int)default_number(name) : (int)l;
}

int cfg_bool(const char *name) {
    const char *v = cfg_value(name);
    return strcmp(v, "0") != 0 && switch_of(v) != 0;
}

static const char *at_start[sizeof metas / sizeof *metas];

static const char *meta_value(size_t i) {
    char name[80];
    snprintf(name, sizeof name, PREFIX "%s", metas[i].key);
    return cfg_value(name);
}

static void start_values(void) {
    for (size_t i = 0; i < sizeof metas / sizeof *metas; i++)
        if (!metas[i].live) at_start[i] = strdup(meta_value(i));
}

static int pending_at(size_t i) {
    char name[80];
    if (!at_start[i]) return 0;
    snprintf(name, sizeof name, PREFIX "%s", metas[i].key);
    return !cfg_from_env(name) && strcmp(at_start[i], cfg_value(name)) != 0;
}

int cfg_pending(const char *key) {
    const CfgMeta *m = cfg_meta(key);
    return m ? pending_at((size_t)(m - metas)) : 0;
}

int cfg_pending_for(const char *key, const char *value) {
    const CfgMeta *m = cfg_meta(key);
    if (!m || !value) return 0;
    size_t i = (size_t)(m - metas);
    char name[80];
    if (!at_start[i]) return 0;
    snprintf(name, sizeof name, PREFIX "%s", metas[i].key);
    return !cfg_from_env(name) && strcmp(at_start[i], value) != 0;
}

int cfg_pending_count(void) {
    int n = 0;
    for (size_t i = 0; i < sizeof metas / sizeof *metas; i++) n += pending_at(i);
    return n;
}

typedef struct { const char *s; size_t n; } Line;

static int line_count(const char *p, size_t size) {
    int n = 0;
    for (size_t i = 0; i < size; i++) n += p[i] == '\n';
    return n + 1;
}

static void line_text(const Line *l, int first, char *out, size_t cap) {
    size_t n = l->n;
    const char *s = l->s;
    if (first && n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3, n -= 3;
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    char *t = trim(out);
    memmove(out, t, strlen(t) + 1);
}

static int line_key(const char *text, int comment, char *key, size_t cap) {
    if (comment) {
        if (*text != '#' && *text != ';') return 0;
        text++;
        while (*text == ' ' || *text == '\t') text++;
    }
    if (!*text || *text == '#' || *text == ';' || *text == '[') return 0;
    const char *eq = strchr(text, '=');
    if (!eq) return 0;
    size_t n = (size_t)(eq - text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t')) n--;
    if (!n || n >= cap) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)text[i]) && text[i] != '_') return 0;
    char tmp[128];
    memcpy(tmp, text, n);
    tmp[n] = 0;
    snprintf(key, cap, "%s", norm_key(tmp));
    return 1;
}

static int line_section(const char *text, char *out, size_t cap) {
    if (*text != '[') return 0;
    const char *e = strchr(text, ']');
    if (!e) return 0;
    char tmp[128];
    size_t n = (size_t)(e - text - 1);
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, text + 1, n);
    tmp[n] = 0;
    snprintf(out, cap, "%s", trim(tmp));
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
    return 1;
}

typedef struct { char *buf; size_t n, cap; } Out;
static void put(Out *o, const char *s, size_t n) {
    if (o->n + n + 1 > o->cap) {
        o->cap = (o->n + n + 1) * 2;
        o->buf = realloc(o->buf, o->cap);
        if (!o->buf) rt_fatal("cfg: out of memory writing the settings file");
    }
    memcpy(o->buf + o->n, s, n);
    o->n += n;
}
static void puts_(Out *o, const char *s) { put(o, s, strlen(s)); }

static const char *section_of(const char *key) {
    for (size_t i = 0; i < sizeof sections / sizeof *sections; i++)
        if (!strcmp(sections[i].key, key)) return sections[i].section;
    if (!strncmp(key, "CHEAT_", 6)) return "cheats";
    return NULL;
}

int cfg_has_section(const char *key) { return section_of(key) != NULL; }

static const char *section_for(const char *key) {
    const char *sec = section_of(key);
    return sec ? sec : "misc";
}

static void put_value(Out *o, const char *v) {
    size_t n = strlen(v);
    int quote = n && (v[0] == ' ' || v[0] == '\t' || v[n - 1] == ' ' || v[n - 1] == '\t' ||
                      (n >= 2 && v[0] == '"' && v[n - 1] == '"'));
    if (quote) puts_(o, "\"");
    puts_(o, v);
    if (quote) puts_(o, "\"");
}

static void put_entry(Out *o, const CfgEntry *e, const char *eol) {
    char k[64];
    snprintf(k, sizeof k, "%s", e->key);
    for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
    puts_(o, k);
    puts_(o, " = ");
    put_value(o, e->value);
    puts_(o, eol);
}

static void park_held(unsigned char *parked, int n) {
    static int test_logged;
    char names[256];
    int tests = 0, len = 0;
    names[0] = 0;
    for (int i = 0; i < n; i++) {
        parked[i] = 0;
        if (!entries[i].held && !entries[i].test) continue;
        if (entries[i].held && (entries[i].dirty || entries[i].removed))
            rt_log("cfg: save leaves out %s: the settings menu previews it and it is not applied yet", entries[i].key);
        else if (entries[i].dirty || entries[i].removed) {
            if (len < (int)sizeof names - 40)
                len += snprintf(names + len, sizeof names - (size_t)len, "%s%s", tests ? ", " : "", entries[i].key);
            tests++;
        }
        parked[i] = (unsigned char)(1 | (entries[i].dirty ? 2 : 0) | (entries[i].removed ? 4 : 0));
        entries[i].dirty = entries[i].removed = 0;
    }
    if (tests && tests != test_logged)
        rt_log("cfg: menu test values not saved: %d key(s): %s%s", tests, names, len >= (int)sizeof names - 40 ? ", ..." : "");
    test_logged = tests;
}

static void unpark_held(const unsigned char *parked, int n) {
    for (int i = 0; i < n; i++)
        if (parked[i]) entries[i].dirty = (parked[i] & 2) != 0, entries[i].removed = (parked[i] & 4) != 0;
}

int cfg_save_to(const char *path) {
    plat_mutex_lock(&set_lock);
    int n = n_entries, any = 0;
    unsigned char parked[CFG_MAX_KEYS];
    park_held(parked, n);
    for (int i = 0; i < n; i++) any |= entries[i].dirty | entries[i].removed;
    if (!any) {
        unpark_held(parked, n);
        __atomic_store_n(&save_ok, 1, __ATOMIC_RELEASE);
        plat_mutex_unlock(&set_lock);
        return 1;
    }
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    if (!data && plat_file_exists(path) && plat_file_size(path) != 0) {
        rt_log("cfg: cannot read %s, nothing written", path);
        unpark_held(parked, n);
        __atomic_store_n(&save_ok, 0, __ATOMIC_RELEASE);
        plat_mutex_unlock(&set_lock);
        return 0;
    }
    const char *text = data ? (const char *)data : "";
    int nl = data ? line_count(text, size) : 0;
    Line *lines = calloc((size_t)nl + 1, sizeof *lines);
    int *replace = malloc(sizeof(int) * ((size_t)nl + 1)), *after_key = malloc(sizeof(int) * (size_t)(n + 1));
    char *drop = calloc((size_t)nl + 1, 1);
    if (!lines || !replace || !after_key || !drop) rt_fatal("cfg: out of memory writing the settings file");
    for (int i = 0, at = 0; i < nl; i++) {
        size_t start = (size_t)at;
        while ((uint64_t)at < size && text[at] != '\n') at++;
        if ((uint64_t)at < size) at++;
        lines[i] = (Line){text + start, (size_t)at - start};
        replace[i] = -1;
    }
    if (nl && !lines[nl - 1].n) nl--;
    const char *eol = data && strstr(text, "\r\n") ? "\r\n" : "\n";
    char t[1024], k[128], sec[128];
    for (int e = 0; e < n; e++) {
        after_key[e] = -2;
        if (entries[e].removed)
            for (int i = 0; i < nl; i++) {
                line_text(&lines[i], i == 0, t, sizeof t);
                if (line_key(t, 0, k, sizeof k) && !strcmp(k, entries[e].key)) drop[i] = 1;
            }
        if (!entries[e].dirty) continue;
        int last = -1;
        for (int i = 0; i < nl; i++) {
            line_text(&lines[i], i == 0, t, sizeof t);
            if (line_key(t, 0, k, sizeof k) && !strcmp(k, entries[e].key)) last = i;
        }
        if (last >= 0) {
            replace[last] = e;
            continue;
        }
        const char *want = section_for(entries[e].key);
        int in = 0, example = -1, tail = -1;
        for (int i = 0; i < nl && want; i++) {
            line_text(&lines[i], i == 0, t, sizeof t);
            if (line_section(t, sec, sizeof sec)) {
                in = !strcmp(sec, want);
                if (in) tail = i;
                continue;
            }
            if (!in) continue;
            if (*t) tail = i;
            if (line_key(t, 1, k, sizeof k) && !strcmp(k, entries[e].key)) example = i;
        }
        after_key[e] = example >= 0 ? example : tail;
    }
    Out o = {0};
    for (int i = 0; i < nl; i++) {
        if (drop[i]) {
        } else if (replace[i] >= 0) {
            const char *s = lines[i].s;
            size_t len = lines[i].n;
            if (i == 0 && len >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) put(&o, s, 3), s += 3, len -= 3;
            const char *eq = memchr(s, '=', len);
            size_t kn = (size_t)(eq - s);
            while (kn && (s[kn - 1] == ' ' || s[kn - 1] == '\t')) kn--;
            put(&o, s, kn);
            puts_(&o, " = ");
            put_value(&o, entries[replace[i]].value);
            int had_eol = len && s[len - 1] == '\n';
            if (had_eol) puts_(&o, eol);
        } else {
            put(&o, lines[i].s, lines[i].n);
        }
        for (int e = 0; e < n; e++)
            if (after_key[e] == i) {
                if (o.n && o.buf[o.n - 1] != '\n') puts_(&o, eol);
                put_entry(&o, &entries[e], eol);
            }
    }
    for (int e = 0; e < n; e++) {
        if (after_key[e] != -1) continue;
        const char *want = section_for(entries[e].key);
        if (o.n && o.buf[o.n - 1] != '\n') puts_(&o, eol);
        if (want) {
            if (o.n) puts_(&o, eol);
            puts_(&o, "[");
            puts_(&o, want);
            puts_(&o, "]");
            puts_(&o, eol);
        }
        put_entry(&o, &entries[e], eol);
        for (int f = e + 1; f < n && want; f++)
            if (after_key[f] == -1 && section_for(entries[f].key) && !strcmp(section_for(entries[f].key), want)) {
                put_entry(&o, &entries[f], eol);
                after_key[f] = -3;
            }
    }
    int ok = plat_file_write_atomic(path, o.buf ? o.buf : "", o.n);
    if (ok) {
        char names[256];
        int nn = 0, len = 0;
        names[0] = 0;
        for (int e = 0; e < n; e++)
            if (entries[e].dirty || entries[e].removed) {
                if (len < (int)sizeof names - 40)
                    len += snprintf(names + len, sizeof names - (size_t)len, "%s%s%s", nn ? ", " : "", entries[e].key,
                                    entries[e].removed ? " (removed)" : "");
                nn++;
            }
        rt_log("cfg: wrote %s: %d key(s): %s%s", path, nn, names, len >= (int)sizeof names - 40 ? ", ..." : "");
        for (int e = 0; e < n; e++) entries[e].dirty = entries[e].removed = 0;
    } else
        rt_log("cfg: cannot write %s", path);
    unpark_held(parked, n);
    __atomic_store_n(&save_ok, ok ? 1 : 0, __ATOMIC_RELEASE);
    free(o.buf);
    free(drop);
    free(after_key);
    free(replace);
    free(lines);
    free(data);
    plat_mutex_unlock(&set_lock);
    return ok;
}

static char ini_path[1024];

static int find_ini(void) {
    if (ini_path[0]) return 1;
    char path[1024], legacy[1024];
    if (!plat_exe_path(path, sizeof path)) return 0;
    char *cut = plat_last_sep(path);
    if (!cut) cut = strrchr(path, '/');
    if (!cut || cut + 13 >= path + sizeof path) return 0;
    strcpy(cut + 1, "nx2navi.ini");
    snprintf(legacy, sizeof legacy, "%s", path);
    strcpy(cut + 1, "nx2saria.ini");
    int have = plat_file_exists(path), old = plat_file_exists(legacy);
    if (!have && old) {
        int r = plat_fs_rename(legacy, path, 1);
        if (r == PLAT_FS_OK) {
            rt_log("cfg: renamed %s to nx2saria.ini, the settings file's name since NX2Saria: every setting in it is kept",
                   legacy);
        } else {
            rt_log("cfg: %s could not be renamed to nx2saria.ini (%s): it is read and written under its old name", legacy,
                   r == PLAT_FS_EXISTS ? "nx2saria.ini appeared meanwhile" : r == PLAT_FS_BUSY ? "the file is in use"
                                                                                  : "the rename was refused");
            snprintf(path, sizeof path, "%s", legacy);
        }
    } else if (have && old) {
        rt_log("cfg: %s is beside nx2saria.ini and is not read: nx2saria.ini is the settings file", legacy);
    }
    snprintf(ini_path, sizeof ini_path, "%s", path);
    return 1;
}

int cfg_save(void) {
    if (bugcap_replay_dir()) return 1;
    if (find_ini()) return cfg_save_to(ini_path);
    __atomic_store_n(&save_ok, 0, __ATOMIC_RELEASE);
    return 0;
}

static void log_file_settings(const char *file) {
    char line[1024];
    size_t k = 0;
    for (int i = 0; i < n_entries; i++) {
        if (!entries[i].value || !is_known(entries[i].key)) continue;
        char key[64];
        snprintf(key, sizeof key, "%s", entries[i].key);
        for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
        const char *value = cfg_private_key(entries[i].key) && entries[i].value[0] ? CFG_PRIVATE_SHOWN : entries[i].value;
        size_t vl = strlen(value);
        if (vl > 200) vl = 200;
        if (k && k + strlen(key) + vl + 5 > 900) {
            rt_log("cfg: %s sets: %s", file, line);
            k = 0;
        }
        k += (size_t)snprintf(line + k, sizeof line - k, "%s%s = %.*s", k ? ", " : "", key, (int)vl, value);
    }
    if (k) rt_log("cfg: %s sets: %s", file, line);
    if (n_unknown)
        rt_log("cfg: %s has %d key%s that %s no setting of this version and %s nothing: %s%s", file, n_unknown,
               n_unknown == 1 ? "" : "s", n_unknown == 1 ? "is" : "are", n_unknown == 1 ? "does" : "do", unknown_keys,
               unknown_cut ? ", ..." : "");
}

typedef struct { char *key, *value; int line; } FileKey;
typedef struct { FileKey k[CFG_MAX_KEYS]; int n; } FileKeys;

static const char *file_get(const FileKeys *f, const char *key) {
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->k[i].key, key)) return f->k[i].value;
    return NULL;
}

static void file_sink(char *key, char *value, int line, void *ctx) {
    FileKeys *f = ctx;
    older_file_key(key, line);
    key = norm_key(key);
    value = cut_comment(key, value, line);
    size_t vl = strlen(value);
    if (vl >= 2 && value[0] == '"' && value[vl - 1] == '"') value[vl - 1] = 0, value++;
    if (!*key || !*value) return;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->k[i].key, key)) {
            free(f->k[i].value);
            f->k[i].value = strdup(value);
            f->k[i].line = line;
            return;
        }
    if (f->n < CFG_MAX_KEYS) f->k[f->n++] = (FileKey){strdup(key), strdup(value), line};
}

static void file_migrate(FileKeys *f) {
    const char *layout = file_get(f, "PAD_LAYOUT");
    if (!layout || strcasecmp(layout, "xbox_prompts")) return;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->k[i].key, "PAD_LAYOUT")) free(f->k[i].value), f->k[i].value = strdup("nintendo");
    if (!file_get(f, "PROMPTS") && f->n < CFG_MAX_KEYS) f->k[f->n++] = (FileKey){strdup("PROMPTS"), strdup("xbox"), 0};
}

static int whole_in(const char *v, long lo, long hi) {
    char *end;
    long l = strtol(v, &end, 10);
    return *v && !*end && l >= lo && l <= hi;
}

static int size_in(const char *v, long min_w, long min_h, long max_w, long max_h) {
    char *end;
    long w = strtol(v, &end, 10);
    if (end == v || (*end != 'x' && *end != 'X')) return 0;
    const char *h_at = end + 1;
    long h = strtol(h_at, &end, 10);
    return end != h_at && !*end && w >= min_w && w <= max_w && h >= min_h && h <= max_h;
}

static int listed_exact(const CfgMeta *m, const char *v) {
    if (!m || !m->values) return 1;
    for (const char *p = m->values; *p;) {
        size_t n = strcspn(p, "|");
        if (strlen(v) == n && !strncmp(p, v, n)) return 1;
        p += n + (p[n] == '|');
    }
    return 0;
}

static int value_ok(const char *key, const char *v) {
    if (!strcmp(key, "LANG") || !strcmp(key, "OPERATION_MODE")) return listed_exact(cfg_meta(key), v);
    if (!strcmp(key, "FPS_LIMIT")) return !strcasecmp(v, "display") || !strcasecmp(v, "unlimited") || whole_in(v, 30, 240);
    if (!strcmp(key, "RENDER_SCALE")) return !strcasecmp(v, "game") || whole_in(v, 25, 400);
    if (!strcmp(key, "WINDOW_SIZE")) return size_in(v, 640, 360, 7680, 4320);
    if (!strcmp(key, "OUTPUT_RES")) return size_in(v, 1, 1, 16384, 16384);
    if (!strcmp(key, "APP_POOL")) {
        char *end;
        return *v && strtoull(v, &end, 0) != 0 && !*end;
    }
    const CfgMeta *m = cfg_meta(key);
    char buf[160];
    return !m || !m->values || !misfit(m->values, v, buf, sizeof buf);
}

static void log_reload(const char *key, const char *value, const char *note) {
    rt_log("cfg: reload %s = %s%s", key, value ? value : "(default)", note);
}

static int env_sets(const char *key) {
    char var[128];
    snprintf(var, sizeof var, PREFIX "%s", key);
    return cfg_env(var) != NULL;
}

static void reload_one(CfgReloadResult *r, CfgEntry *e, const char *key, const char *want, int line) {
    if (env_sets(key)) {
        log_reload(key, want, " (overridden by environment)");
        r->env++;
        return;
    }
    if (e && (e->held || e->dirty || e->removed)) {
        rt_log("cfg: reload %s skipped: changed in the menu, not saved yet", key);
        r->kept++;
        return;
    }
    if (want && !value_ok(key, want)) {
        rt_log("cfg: reload %s = '%s' rejected: invalid", key, want);
        r->rejected++;
        return;
    }
    if (e) {
        if (e->value) kept_n++, kept_bytes += strlen(e->value) + 1;
        __atomic_store_n(&e->value, want ? strdup(want) : NULL, __ATOMIC_RELEASE);
        entry_line[e - entries] = want ? line : 0;
    } else {
        if (n_entries == CFG_MAX_KEYS) {
            full_set(key, "is not kept");
            return;
        }
        entry_line[n_entries] = line;
        entries[n_entries] = (CfgEntry){strdup(key), strdup(want), 0, 0, 0, 0};
        __atomic_store_n(&n_entries, n_entries + 1, __ATOMIC_RELEASE);
    }
    if (!cfg_supported(key)) {
        log_reload(key, want, "");
        rt_log("cfg: %s has no effect on this game version", key);
        return;
    }
    const CfgMeta *m = cfg_meta(key);
    int restart = m && !m->live;
    log_reload(key, want, restart ? " (takes effect after restart)" : "");
    r->changed++;
    r->restart += restart;
}

static int preset_parse(const char *v);
static void preset_follow(int before);

int cfg_reload_file(CfgReloadResult *r) {
    CfgReloadResult none = {0};
    if (!r) r = &none;
    *r = (CfgReloadResult){0};
    if (bugcap_replay_dir()) {
        rt_log("cfg: reload: not run: this run replays a bug capture's settings and does not read the player's settings file");
        return 0;
    }
    FileKeys *f = calloc(1, sizeof *f);
    if (!f || !find_ini() || !plat_file_exists(ini_path) || !parse_ini(ini_path, file_sink, f)) {
        rt_log("cfg: reload: cannot read the settings file, settings unchanged");
        free(f);
        return 0;
    }
    check_end();
    file_migrate(f);
    plat_mutex_lock(&set_lock);
    int preset_before = preset_parse(raw_get(PREFIX "PRESET"));
    int n = n_entries;
    for (int i = 0; i < n; i++) {
        CfgEntry *e = &entries[i];
        const char *now = e->value, *want = file_get(f, e->key);
        if (now ? want && !strcmp(now, want) : !want) continue;
        int line = 0;
        for (int k = 0; k < f->n; k++)
            if (!strcmp(f->k[k].key, e->key)) line = f->k[k].line;
        reload_one(r, e, e->key, want, line);
    }
    for (int i = 0; i < f->n; i++) {
        int have = 0;
        for (int j = 0; j < n && !have; j++) have = !strcmp(entries[j].key, f->k[i].key);
        if (!have) reload_one(r, NULL, f->k[i].key, f->k[i].value, f->k[i].line);
    }
    if (r->changed) __atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&set_lock);
    for (int i = 0; i < f->n; i++) free(f->k[i].key), free(f->k[i].value);
    free(f);
    if (r->changed) preset_follow(preset_before);
    rt_log("cfg: reload: %d changed (%d after restart), %d rejected, %d overridden by environment, %d kept (unsaved menu change)",
           r->changed, r->restart, r->rejected, r->env, r->kept);
    return 1;
}

#define PRESET_KEYS_MAX 32
static PlatMutex preset_lock = PLAT_MUTEX_INIT;
static struct {
    char path[1024];
    char text[2048];
    char logged[2048];
    char chosen_on[160];
    char noticed[16];
    char fp[160];
    char why[192];
    PresetHw gpu;
    int rec;
    int write;
    int probing;
    int hw_changed;
    int n_keys, keys_done;
    const char *keys[PRESET_KEYS_MAX];
} PS = {.rec = PRESET_HIGH};

static void preset_keys_init(void) {
    if (__atomic_load_n(&PS.keys_done, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&preset_lock);
    if (!PS.keys_done) {
        for (size_t i = 0; i < PRESET_ROW_COUNT; i++) {
            if (!preset_applies(PRESET_ROWS[i].when, &preset_hw)) continue;
            int have = 0;
            for (int k = 0; k < PS.n_keys && !have; k++) have = !strcmp(PS.keys[k], PRESET_ROWS[i].key);
            if (have) continue;
            if (PS.n_keys == PRESET_KEYS_MAX) rt_fatal("cfg: more than %d keys in preset_pick.h PRESET_ROWS", PRESET_KEYS_MAX);
            PS.keys[PS.n_keys++] = PRESET_ROWS[i].key;
        }
        __atomic_store_n(&PS.keys_done, 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&preset_lock);
}

static int preset_parse(const char *v) {
    if (!v || !*v || !strcasecmp(v, "recommended")) return -1;
    for (int t = 0; t < PRESET_TIERS; t++)
        if (!strcasecmp(v, preset_tier_names[t])) return t;
    return -2;
}

static const char *gpu_type_name(int type) {
    return type == PRESET_GPU_DISCRETE ? "discrete" : type == PRESET_GPU_INTEGRATED ? "integrated"
         : type == PRESET_GPU_CPU ? "cpu" : "other";
}

static int state_number(const char *v, uint32_t *out) {
    char *end;
    unsigned long n = strtoul(v, &end, 0);
    if (!*v || *end || n > 0xffffffffu) return 0;
    *out = (uint32_t)n;
    return 1;
}

static void state_path_init(void) {
    char path[1024];
    if (!plat_exe_path(path, sizeof path)) return;
    char *cut = plat_last_sep(path);
    if (!cut) cut = strrchr(path, '/');
    if (!cut || (size_t)(cut + 1 - path) + sizeof "preset-state.txt" > sizeof PS.path) return;
    snprintf(PS.path, sizeof PS.path, "%.*spreset-state.txt", (int)(cut + 1 - path), path);
}

static void state_read(void) {
    if (!PS.path[0]) return;
    uint64_t size = 0;
    char *data = (char *)plat_file_read(PS.path, &size);
    if (!data) return;
    snprintf(PS.text, sizeof PS.text, "%s", data);
    memcpy(PS.logged, PS.text, sizeof PS.logged);
    PresetHw *g = &PS.gpu;
    enum { G_NAME = 1, G_VENDOR = 2, G_DEVICE = 4, G_DRIVER = 8, G_TYPE = 16, G_VRAM = 32, G_MESH = 64, G_ALL = 127 };
    unsigned have = 0;
    for (char *line = data, *end = data + size, *next; line < end; line = next) {
        next = memchr(line, '\n', (size_t)(end - line));
        if (next) *next++ = 0;
        else next = end;
        char *eq = strchr(line, '=');
        if (*line == '#' || !eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        uint32_t n;
        if (!strcmp(k, "chosen_on")) snprintf(PS.chosen_on, sizeof PS.chosen_on, "%s", v);
        else if (!strcmp(k, "noticed")) snprintf(PS.noticed, sizeof PS.noticed, "%s", v);
        else if (!strcmp(k, "hardware")) snprintf(PS.fp, sizeof PS.fp, "%s", v);
        else if (!strcmp(k, "probing")) PS.probing = atoi(v) != 0;
        else if (!strcmp(k, "gpu_name") && *v) have |= G_NAME, snprintf(g->gpu_name, sizeof g->gpu_name, "%s", v);
        else if (!strcmp(k, "gpu_vendor") && state_number(v, &n) && n) have |= G_VENDOR, g->vendor = n;
        else if (!strcmp(k, "gpu_device") && state_number(v, &n)) have |= G_DEVICE, g->device = n;
        else if (!strcmp(k, "gpu_driver") && state_number(v, &n)) have |= G_DRIVER, g->driver = n;
        else if (!strcmp(k, "gpu_vram_mib") && state_number(v, &n)) have |= G_VRAM, g->vram_mib = n;
        else if (!strcmp(k, "gpu_mesh") && (!strcmp(v, "0") || !strcmp(v, "1"))) have |= G_MESH, g->mesh = v[0] == '1';
        else if (!strcmp(k, "gpu_type")) {
            int t = !strcmp(v, "discrete") ? PRESET_GPU_DISCRETE : !strcmp(v, "integrated") ? PRESET_GPU_INTEGRATED
                  : !strcmp(v, "cpu") ? PRESET_GPU_CPU : !strcmp(v, "other") ? PRESET_GPU_OTHER : -1;
            if (t >= 0) have |= G_TYPE, g->gpu_type = t;
        }
    }
    free(data);
    if (have == G_ALL) g->have_gpu = 1;
    else memset(g, 0, sizeof *g);
}

static void state_write(int quiet) {
    if (!PS.path[0] || !PS.write) return;
    char t[sizeof PS.text];
    int n = snprintf(t, sizeof t, "# NX2Saria: what the quality presets last saw of this PC. The game writes this file; "
                                  "the settings are in nx2saria.ini.\n");
    if (PS.fp[0] && n < (int)sizeof t) n += snprintf(t + n, sizeof t - (size_t)n, "hardware = %s\n", PS.fp);
    if (PS.chosen_on[0] && n < (int)sizeof t) n += snprintf(t + n, sizeof t - (size_t)n, "chosen_on = %s\n", PS.chosen_on);
    if (PS.noticed[0] && n < (int)sizeof t) n += snprintf(t + n, sizeof t - (size_t)n, "noticed = %s\n", PS.noticed);
    const PresetHw *g = &PS.gpu;
    if (g->have_gpu && n < (int)sizeof t)
        n += snprintf(t + n, sizeof t - (size_t)n,
                      "gpu_name = %s\ngpu_vendor = 0x%04x\ngpu_device = 0x%04x\ngpu_driver = 0x%x\ngpu_type = %s\n"
                      "gpu_vram_mib = %u\ngpu_mesh = %d\n",
                      g->gpu_name, g->vendor, g->device, g->driver, gpu_type_name(g->gpu_type), g->vram_mib, g->mesh);
    if (PS.probing && n < (int)sizeof t) n += snprintf(t + n, sizeof t - (size_t)n, "probing = 1\n");
    if (n >= (int)sizeof t || !strcmp(t, PS.text)) return;
    if (plat_file_write_atomic(PS.path, t, (size_t)n)) {
        memcpy(PS.text, t, (size_t)n + 1);
        if (quiet || !strcmp(t, PS.logged)) return;
        memcpy(PS.logged, t, (size_t)n + 1);
        rt_log("preset: wrote %s", PS.path);
    } else {
        rt_log("preset: cannot write %s", PS.path);
    }
}

static void state_write_main(void *arg) {
    (void)arg;
    plat_mutex_lock(&preset_lock);
    state_write(0);
    plat_mutex_unlock(&preset_lock);
}

static void state_write_later(void) {
    if (!plat_thread_spawn(state_write_main, NULL, "preset state", 256u << 10)) state_write_main(NULL);
}

static void preset_probe_begin(void) {
    rt_log("preset: probing the GPU (a Vulkan instance; preset_probe = 0 turns it off)");
    if (!PS.write) return;
    PS.probing = 1;
    state_write(1);
}

static const char *deck_value(const char *key) {
    if (profile_of())
        for (size_t i = 0; i < sizeof DECK / sizeof *DECK; i++)
            if (!strcasecmp(DECK[i].key, key)) return DECK[i].value;
    return NULL;
}

static int same_value(const char *a, const char *b) {
    if (!a || !b) return a == b;
    if (!strcasecmp(a, b)) return 1;
    char *ea, *eb;
    double x = strtod(a, &ea), y = strtod(b, &eb);
    return ea != a && eb != b && !*ea && !*eb && x == y;
}

static void preset_log_keys(int tier) {
    int any = 0;
    for (int k = 0; k < PS.n_keys; k++) {
        const char *key = PS.keys[k], *v = preset_value(tier, &preset_hw, key);
        if (!v) continue;
        any = 1;
        char name[80];
        snprintf(name, sizeof name, PREFIX "%s", key);
        const char *set = raw_get(name), *deck = deck_value(key);
        if (set && *set) {
            const char *from = cfg_env(name) ? "environment" : "settings file";
            if (same_value(switch_word(name, set), v)) rt_log("preset:   %s = %s (%s, the same as the preset)", key, set, from);
            else rt_log("preset:   %s = %s (%s; the preset's %s stays unused)", key, set, from, v);
        } else if (deck) {
            if (same_value(deck, v)) rt_log("preset:   %s = %s (device profile)", key, deck);
            else rt_log("preset:   %s = %s (device profile; the preset's %s stays unused)", key, deck, v);
        } else {
            rt_log("preset:   %s = %s (preset)", key, v);
        }
    }
    if (!any) rt_log("preset: %s sets nothing: the game as shipped", preset_tier_names[tier]);
}

static void preset_init(int game) {
    char skipped[192], note[192];
    double ms;
    PresetHw hw;
    const char *replay = bugcap_replay_dir();
    PS.write = game && !replay;
    preset_keys_init();
    state_path_init();
    state_read();
    const char *probe_set = cfg_get_now(PREFIX "PRESET_PROBE");
    const char *no_probe = !game ? "a tool mode, not the game itself"
                         : probe_set && !strcmp(probe_set, "0") ? "preset_probe = 0"
                         : PS.probing ? "the last start's probe did not finish (preset-state.txt says probing = 1); "
                                        "delete that line to probe again"
                         : NULL;
    hw_detect(&hw, no_probe, preset_probe_begin, skipped, sizeof skipped, note, sizeof note, &ms);
    if (!skipped[0]) PS.probing = 0;
    int cached = 0;
    if (hw.have_gpu) {
        PS.gpu = hw;
    } else if (skipped[0] && PS.gpu.have_gpu) {
        const PresetHw *g = &PS.gpu;
        hw.have_gpu = 1, hw.vendor = g->vendor, hw.device = g->device, hw.driver = g->driver;
        hw.gpu_type = g->gpu_type, hw.vram_mib = g->vram_mib, hw.mesh = g->mesh;
        snprintf(hw.gpu_name, sizeof hw.gpu_name, "%s", g->gpu_name);
#ifdef _WIN32
        preset_gpu_caps(&hw, 1);
#else
        preset_gpu_caps(&hw, 0);
#endif
        cached = 1;
    }
    preset_hw = hw;
    __atomic_store_n(&PS.keys_done, 0, __ATOMIC_RELEASE);
    PS.n_keys = 0;
    preset_keys_init();
    char gpu[512], tail[256];
    if (hw.have_gpu)
        snprintf(gpu, sizeof gpu, "GPU \"%s\" (vendor 0x%04x device 0x%04x, %s, %u MiB video memory, mesh shaders %s, "
                                  "DLSS %s, FSR 4 %s)",
                 hw.gpu_name, hw.vendor, hw.device, gpu_type_name(hw.gpu_type), hw.vram_mib, hw.mesh ? "yes" : "no",
                 hw.dlss ? "yes" : "no", hw.fsr4 ? "yes" : "no");
    else
        snprintf(gpu, sizeof gpu, "GPU none (%s)", note[0] ? note : skipped[0] ? "not probed, and no earlier probe kept" : "unknown");
    if (skipped[0]) snprintf(tail, sizeof tail, "probe skipped: %s%s", skipped, cached ? "; the GPU as the last probe saw it" : "");
    else snprintf(tail, sizeof tail, "probe %.1f ms", ms);
    rt_log("preset: hardware: %s; CPU %d cores / %d threads; RAM %.1f GiB; handheld %s; %s", gpu, hw.cores, hw.threads,
           hw.ram_mib / 1024.0, hw.handheld[0] ? hw.handheld : "none", tail);
    PS.rec = preset_pick(&hw, PS.why, sizeof PS.why);
    rt_log("preset: recommended %s: %s", preset_tier_names[PS.rec], PS.why);
    if (!strcmp(hw.handheld, "Steam Deck"))
        rt_log("preset: Steam Deck: the GPU clock is left to the system's power management (no peak clock request)");
    preset_fingerprint(&hw, PS.fp, sizeof PS.fp);

    char name[] = PREFIX "PRESET";
    const char *v = raw_get(name);
    int want = preset_parse(v), tier = PS.rec;
    if (want == -2) {
        rt_log("preset: '%s' is no preset: recommended", v);
        want = -1;
    }
    char source[96] = "recommended";
    if (replay) {
        tier = PRESET_HIGH, want = -1;
        snprintf(source, sizeof source, "a bug capture's replay: its settings.ini holds the capture's preset values");
    } else if (want >= 0) {
        const int env = cfg_env(name) != NULL;
        if (!env && PS.chosen_on[0] && hw.have_gpu && !preset_same_hardware(PS.chosen_on, PS.fp)) {
            __atomic_store_n(&PS.hw_changed, 1, __ATOMIC_RELEASE);
            snprintf(source, sizeof source, "recommended: the hardware changed since %s was chosen", preset_tier_names[want]);
        } else {
            tier = want;
            snprintf(source, sizeof source, "%s", env ? "environment" : "settings file");
            if (!env && !PS.chosen_on[0] && hw.have_gpu) snprintf(PS.chosen_on, sizeof PS.chosen_on, "%s", PS.fp);
        }
    }
    __atomic_store_n(&preset_tier, tier, __ATOMIC_RELEASE);
    __atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);
    rt_log("preset: in use %s (%s)", preset_tier_names[tier], source);
    if (PS.hw_changed)
        rt_log("preset: %s was chosen on %s, and this PC is %s (vendor:device:driver:video GiB:threads:RAM GiB:handheld); "
               "choosing a preset in the settings menu keeps it again", preset_tier_names[want], PS.chosen_on, PS.fp);
    else if (want >= 0 && want != PS.rec)
        rt_log("preset: %s chosen; the recommended for this PC is %s", preset_tier_names[want], preset_tier_names[PS.rec]);
    preset_log_keys(tier);
    if (PS.probing)
        rt_log("preset: the probe of an earlier start did not finish (a driver that crashed or hung in it, or a write "
               "that failed): no probe this start; the mark goes once the game's Vulkan device is chosen, or delete the "
               "line 'probing = 1' from %s", PS.path[0] ? PS.path : "preset-state.txt");
    state_write(0);
}

static void preset_follow(int before) {
    if (__atomic_load_n(&preset_tier, __ATOMIC_ACQUIRE) < 0 || bugcap_replay_dir()) return;
    int want = preset_parse(raw_get(PREFIX "PRESET"));
    if (want == -2) want = -1;
    if (before == -2) before = -1;
    if (want == before) return;
    rt_log("preset: the settings file now says %s", want >= 0 ? preset_tier_names[want] : "recommended");
    plat_mutex_lock(&preset_lock);
    __atomic_store_n(&PS.hw_changed, 0, __ATOMIC_RELEASE);
    const int record = want >= 0 && preset_hw.have_gpu && strcmp(PS.chosen_on, PS.fp);
    if (record) snprintf(PS.chosen_on, sizeof PS.chosen_on, "%s", PS.fp);
    plat_mutex_unlock(&preset_lock);
    if (record) state_write_later();
    cfg_preset_use(want >= 0 ? want : PS.rec);
}

const char *cfg_preset_name(int tier) { return tier >= 0 && tier < PRESET_TIERS ? preset_tier_names[tier] : "recommended"; }

int cfg_preset_in_use(void) { return __atomic_load_n(&preset_tier, __ATOMIC_ACQUIRE); }

int cfg_preset_recommended(const char **why) {
    if (why) *why = PS.why;
    return PS.rec;
}

int cfg_preset_hw_changed(void) { return __atomic_load_n(&PS.hw_changed, __ATOMIC_ACQUIRE); }

const char *const *cfg_preset_keys(int *n) {
    preset_keys_init();
    *n = PS.n_keys;
    return PS.keys;
}

const char *cfg_preset_value(int tier, const char *key) {
    if (tier < 0 || tier >= PRESET_TIERS || !key) return NULL;
    char k[64];
    snprintf(k, sizeof k, "%s", key);
    return preset_value(tier, &preset_hw, norm_key(k));
}

int cfg_preset_custom(void) {
    int t = __atomic_load_n(&preset_tier, __ATOMIC_ACQUIRE);
    if (t < 0) return 0;
    preset_keys_init();
    for (int k = 0; k < PS.n_keys; k++) {
        char name[80];
        snprintf(name, sizeof name, PREFIX "%s", PS.keys[k]);
        if (cfg_preset_differs(t, name, raw_get(name))) return 1;
    }
    return 0;
}

const char *cfg_default_tier(const char *key, int tier) {
    if (tier < 0 || tier >= PRESET_TIERS) return cfg_default(key);
    char k[64];
    snprintf(k, sizeof k, "%s", key);
    const char *name = norm_key(k), *deck = deck_value(name);
    if (deck) return deck;
    const char *pv = preset_value(tier, &preset_hw, name);
    if (pv) return pv;
    const CfgMeta *m = cfg_meta(key);
    return m ? m->def : NULL;
}

int cfg_preset_differs(int tier, const char *key, const char *value) {
    if (!value || !*value || !cfg_preset_value(tier, key)) return 0;
    char k[64], name[80];
    snprintf(k, sizeof k, "%s", key);
    snprintf(name, sizeof name, PREFIX "%s", norm_key(k));
    return !same_value(switch_word(name, value), cfg_default_tier(key, tier));
}

int cfg_preset_notice_due(void) {
    int t = __atomic_load_n(&preset_tier, __ATOMIC_ACQUIRE);
    if (t < 0 || !PS.write || PS.rec == PRESET_HIGH || t != PS.rec) return 0;
    const char *set = raw_get(PREFIX "PRESET");
    if (set && *set) return 0;
    plat_mutex_lock(&preset_lock);
    int due = strcmp(PS.noticed, preset_tier_names[PS.rec]) != 0;
    plat_mutex_unlock(&preset_lock);
    return due;
}

void cfg_preset_notice_done(void) {
    plat_mutex_lock(&preset_lock);
    snprintf(PS.noticed, sizeof PS.noticed, "%s", preset_tier_names[PS.rec]);
    rt_log("preset: notice shown: recommended %s in use", preset_tier_names[PS.rec]);
    plat_mutex_unlock(&preset_lock);
    state_write_later();
}

void cfg_preset_device_started(void) {
    plat_mutex_lock(&preset_lock);
    const int clear = PS.probing && PS.write;
    if (clear) PS.probing = 0;
    plat_mutex_unlock(&preset_lock);
    if (!clear) return;
    rt_log("preset: the game's Vulkan device is chosen: the probing mark of an earlier start is taken out of %s, and the "
           "next start probes again", PS.path);
    state_write_later();
}

void cfg_preset_chosen(void) {
    plat_mutex_lock(&preset_lock);
    if (preset_hw.have_gpu) snprintf(PS.chosen_on, sizeof PS.chosen_on, "%s", PS.fp);
    else PS.chosen_on[0] = 0;
    __atomic_store_n(&PS.hw_changed, 0, __ATOMIC_RELEASE);
    if (PS.rec >= 0 && PS.rec < PRESET_TIERS) snprintf(PS.noticed, sizeof PS.noticed, "%s", preset_tier_names[PS.rec]);
    state_write(0);
    plat_mutex_unlock(&preset_lock);
}

void cfg_preset_use(int tier) { cfg_preset_use_from(tier, NULL); }

void cfg_preset_use_from(int tier, const char *const *from_values) {
    if (tier < 0 || tier >= PRESET_TIERS) return;
    preset_keys_init();
    plat_mutex_lock(&preset_lock);
    int old = __atomic_load_n(&preset_tier, __ATOMIC_ACQUIRE);
    if (old == tier && !from_values) {
        plat_mutex_unlock(&preset_lock);
        return;
    }
    const char *before[PRESET_KEYS_MAX];
    char name[80];
    for (int k = 0; k < PS.n_keys; k++) {
        snprintf(name, sizeof name, PREFIX "%s", PS.keys[k]);
        before[k] = from_values && from_values[k] ? from_values[k] : cfg_value(name);
    }
    __atomic_store_n(&preset_tier, tier, __ATOMIC_RELEASE);
    __atomic_add_fetch(&version, 1, __ATOMIC_RELEASE);
    const char *from = old >= 0 ? preset_tier_names[old] : "none";
    int changed = 0;
    for (int k = 0; k < PS.n_keys; k++) {
        snprintf(name, sizeof name, PREFIX "%s", PS.keys[k]);
        const char *after = cfg_value(name);
        if (!same_value(before[k], after))
            rt_log("preset: %s -> %s: %s %s -> %s", from, preset_tier_names[tier], PS.keys[k], before[k], after), changed++;
    }
    if (!changed) rt_log("preset: %s -> %s: no setting changes", from, preset_tier_names[tier]);
    plat_mutex_unlock(&preset_lock);
}

static void load_ini(void) {
    const char *replay = bugcap_replay_dir();
    if (replay) {
        char path[1100];
        snprintf(path, sizeof path, "%s/settings.ini", replay);
        int ok = cfg_load(path);
        rt_log(ok ? "cfg: replay of the bug capture in %s: its settings.ini (%d keys) in place of the player's settings file, which this run does not write"
                  : "cfg: replay of the bug capture in %s: no settings.ini there, so every setting has its default (%d keys); "
                    "the player's settings file is not read or written",
               replay, n_entries);
        if (ok) log_file_settings("the capture's settings.ini");
        return;
    }
    if (!find_ini()) return;
    if (!cfg_load(ini_path)) {
        rt_log("cfg: no settings file at %s", ini_path);
        return;
    }
    rt_log("cfg: read %s: %d keys", ini_path, n_entries);
    log_file_settings("settings file");
    for (int i = 0; i < n_entries; i++) {
        char var[128], legacy[128];
        snprintf(var, sizeof var, PREFIX "%s", entries[i].key);
        snprintf(legacy, sizeof legacy, OLD_PREFIX "%s", entries[i].key);
        if (cfg_env(var))
            rt_log("cfg: %s is set in the environment and overrides the settings file", getenv(var) ? var : legacy);
    }
}

void cfg_init(void) {
    load_ini();
    older_env();
    for (size_t i = 0; i < sizeof metas / sizeof *metas; i++) {
        char var[80];
        snprintf(var, sizeof var, PREFIX "%s", metas[i].key);
        const char *e = cfg_env(var);
        if (e && *e) check_value(metas[i].key, e, 0);
    }
    check_end();
    if (profile_of())
        rt_log("cfg: device profile Steam Deck: defaults for the keys the settings file does not set (NX2SARIA_DEVICE_PROFILE=desktop "
               "turns it off)");
    if (lowend_set())
        rt_log("cfg: low_end_gpu is on, and the low-end GPU mode was removed in 0.11.0: the line does nothing, and the game "
               "runs as it does without it. The mode's single settings (mesh_emulation, tex_clobber, foliage, "
               "vram_reserve_mb and the others) still count where the settings file or environment sets them");
}

void cfg_start(int game) {
    preset_init(game);
    start_values();
}

void cfg_sizes(PerfSizes *s) {
    plat_mutex_lock(&set_lock);
    uint64_t n = (uint64_t)n_entries, kn = kept_n, kb = kept_bytes;
    plat_mutex_unlock(&set_lock);
    perf_size(s, "settings", n, CFG_MAX_KEYS, PERF_SIZE_NONE);
    perf_size(s, "replaced setting values kept", kn, 0, kb);
}
