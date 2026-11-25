// The names of the port's own patch sets: every `set <name>` line of recomp/patches-v*.txt, sorted. A report's
// `patches` list is kept only as far as it names these (validate.ts knownPatches); tests/telemetry_patch_sets_check.py
// fails when this list and those files differ.
export const PATCH_SETS: readonly string[] = [
  "amiibo_daily", "aspect", "bounce", "camera_clearance", "cascade", "changebank_hold", "cloth", "cursor", "fade",
  "fov", "fov_menu", "fps60", "freecam", "heap_retry", "menu_mouse", "mouse_direct", "obgm", "occlusion_off",
  "ofollow", "ogohma", "ohorse", "omovie", "opickup", "orecord", "osolver", "osolverfull", "osound", "ostaff",
  "ostep", "output", "owater", "ozora", "pooloff", "quit_menu", "realtime", "res_fixed", "scene", "shadow",
  "sprint", "text_speed", "voice_language", "wide_aspect", "wide_balloon", "wide_bars", "wide_hud", "wide_movie",
  "wide_out", "wide_save", "wide_scene", "zstd",
];
