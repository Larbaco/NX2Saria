// Settings: NX2SARIA_* environment variables and nx2saria.ini; existing NX2NAVI_* variables and nx2navi.ini remain supported.
#pragma once
#include <stddef.h>

// Environment lookup for a setting name: NX2SARIA_* takes precedence over NX2NAVI_* even when empty. Accepts either
// prefixed spelling; non-setting names are passed directly to getenv. Used by the early platform probes too.
const char *cfg_env(const char *name);

// getenv for a setting: the environment variable when set, else the settings-file key (the name without either prefix, any
// case), else the device profile's default (cfg.c: the Steam Deck), else the quality preset's (cfg_preset_*), else NULL. Same string semantics as getenv; the pointer stays valid for the process (a value cfg_set replaces
// is kept). A name without either prefix goes to getenv unchanged. Safe from any thread once cfg_start has returned.
const char *cfg_get(const char *name);

// Reads the selected settings file once, from mem_init after the fixed windows are claimed and before any setting is
// read. Logs the file, its key count, unknown keys and environment overrides. cfg_start follows it.
void cfg_init(void);
// The rest of the settings' start, from mem_init once the whole guest address space is claimed (the quality preset's
// probe loads the Vulkan drivers, whose libraries and allocations must not land in the stack and heap block or the
// alias region): the quality preset in use (cfg.c "quality presets") and then the restart-only settings' values
// (cfg_pending). game: 1 for the game itself (main.c: no tool mode), which probes the GPU (hw_detect.c) and keeps the
// state file preset-state.txt; 0 for a tool mode, which goes by the state file's last probe and writes nothing.
void cfg_start(int game);

// Replaces the loaded keys with those of the file at path (tests). 1 when the file was read, 0 when missing.
// Not thread-safe: nothing else may call cfg_get meanwhile.
int cfg_load(const char *path);

// Reads the selected settings file again (Ctrl+F5, ui/menu.cpp) and applies what changed since it was loaded, without a restart: a line
// changed, added or removed (the key then falls back to its default). Each line is read as cfg_load reads it (a comment
// after a value cut) and each new value is checked first (cfg.c value_ok: the check cfg_load logs at start for values
// outside their rows' sets, and rules for the consumers that take more or stop the game on a bad one); a value that does
// not fit is rejected and the current one stays. Kept as they are, and logged: keys the environment sets, and keys changed in the menu and not
// saved yet or held by its draft (cfg_hold). Applied keys are not marked for cfg_save and cfg_version is bumped once.
// Every key is logged ("cfg: reload ..."). Fills *r (may be NULL): changed counts the keys that now read the new value
// on this game build, restart those of them read at start only. Returns 0 when the file cannot be read, and in a run
// that replays a bug capture's settings (NX2SARIA_CAPTURE_REPLAY), which reads no player settings file. Any thread.
typedef struct { int changed, restart, rejected, env, kept; } CfgReloadResult;
int cfg_reload_file(CfgReloadResult *r);

// Live settings (the in-game menu). cfg_set stores value as the settings-file value of name (NX2SARIA_ prefixed), marks it for
// cfg_save and bumps cfg_version. cfg_get returns it unless the environment sets name. Any thread.
void cfg_set(const char *name, const char *value);
// Bumped by every cfg_set. A consumer that keeps a parsed value compares this with the number it last saw (one atomic
// load) and parses again when it changed.
unsigned cfg_version(void);
// plat_setenv for an NX2SARIA_ variable the process reads back: also bumps cfg_version, so lookups see the new value.
int cfg_setenv(const char *name, const char *value);
// The menu's draft (ui/option_rows.cpp): a live setting is previewed with cfg_set, and Discard puts it back. cfg_state
// reads what cfg_revert needs of name before the first change: the stored value (NULL when none; the pointer stays valid)
// and the CFG_* flags of its entry (unsaved change, line to remove at the next save). cfg_hold(name, 1) keeps the entry
// out of every cfg_save / cfg_save_to until cfg_hold(name, 0) (Apply), so an F10 / F11 / window-close save made while the
// draft is open does not write a previewed value. cfg_revert restores the value and flags cfg_state gave, releases the
// hold and bumps cfg_version. Any thread.
enum { CFG_DIRTY = 1, CFG_REMOVED = 2 };
int cfg_state(const char *name, const char **value);
void cfg_hold(const char *name, int on);
void cfg_revert(const char *name, const char *orig_value, int orig_state);
// 1 when the environment sets name: cfg_set cannot change what cfg_get returns (the menu greys the setting out).
int cfg_from_env(const char *name);
// 1 when the player set name (the environment or settings file), 0 when cfg_get gives a device profile's default or NULL.
int cfg_is_set(const char *name);
// The keys of the file's lines that are no settings of this version, lower-case with ", " between them ("" when there
// are none; a list too long to keep ends early), and how many there are. Such a line does nothing: the log says so at
// start and the settings menu shows it (ui/menu.cpp), so that nobody takes one for the cause of what they see.
const char *cfg_unknown_keys(int *count);
// The device profile in use, "deck" or "desktop" (NX2SARIA_DEVICE_PROFILE, else what the machine is).
const char *cfg_device_profile(void);
// Writes the keys cfg_set changed since the last save into the selected settings file beside the game executable (cfg_save_to: the file at path).
// Only those lines change: the last line of a changed key gets the new value, a missing key goes under its section
// (after the commented example line of the key when the section has one; the section is added at the end of the file
// when missing), comments and every other line stay byte for byte. 1 on success or with nothing to write.
int cfg_save(void);
int cfg_save_to(const char *path);

// Reset (the menu's "Reset all settings to defaults"). Removes the active line of each key (names without NX2SARIA_, any
// case) from the selected settings file at the next cfg_save and its value from memory, so the consumer's own default applies again; the
// commented example lines stay. Keys the environment sets are skipped. Bumps cfg_version. Returns how many it reset.
int cfg_reset(const char *const *keys, int n);
// 0 when the last cfg_save / cfg_save_to failed to write (the values stay set in memory, the next save tries again).
int cfg_last_save_ok(void);
// Every setting with a value, for a bug report ("Copy system info"): the settings file and cfg_set keys (name without
// either prefix, upper case; from_env 0), then the known NX2SARIA_ variables the environment sets (from_env 1).
void cfg_each_active(void (*cb)(const char *key, const char *value, int from_env, void *ctx), void *ctx);

// What the menu knows about a setting. key: without NX2SARIA_, upper case. def: what the consumer uses when the key is
// unset ("" = no fixed default, the consumer picks per machine). values: "0|1", "mailbox|fifo|immediate", a range
// "0..300", or NULL for free text. live: applies at once; the others are read at start.
typedef struct { const char *key, *def, *values; int live; } CfgMeta;
// Whether key (prefix and case ignored) does anything on the running game build (cfg_support.c): 0 for a setting whose
// patch set the build lacks or that only another game reads. The menu hides such rows; 1 for every other key. Call it
// after rt_patches_init.
int cfg_supported(const char *key);
// 1 when key has a settings-file section of its own in cfg.c (cfg_save writes any other key under [misc]).
int cfg_has_section(const char *key);
// Logs "cfg: KEY has no effect on this game version" for each key the settings file or environment sets that is not supported.
void cfg_log_unsupported(void);
// The row of key (prefix and case ignored); NULL for a key the menu does not show.
const CfgMeta *cfg_meta(const char *key);
// The i-th row (0-based, table order); NULL past the last.
const CfgMeta *cfg_meta_at(int i);
// What key reads when the player has not set it: the device profile's value (the Steam Deck's), else the quality preset's
// in use, else the row's default;
// NULL for a key without a row. The menu's "default" mark follows it.
const char *cfg_default(const char *key);
// A restart-only setting (live 0) whose value differs from the one cfg_start kept at start, which the running game still
// uses: 1 while a restart would change it, 0 otherwise and for a live key or one without a row (the menu's "after
// restart" note). cfg_pending_count: how many restart-only settings differ.
int cfg_pending(const char *key);
int cfg_pending_count(void);
// cfg_pending for value instead of the key's own (the settings menu's draft, ui/option_rows.cpp opt_pending_count).
int cfg_pending_for(const char *key, const char *value);

// The setting's value for consumers: the environment or settings-file value, else the CfgMeta default. name has the NX2SARIA_
// prefix, like cfg_get; a name without a row is a bug (rt_fatal). A row with an empty default has no fixed value: its
// consumer reads cfg_get and keeps its own "unset" branch. An empty value counts as unset.
const char *cfg_value(const char *name);
// cfg_value parsed. A set value that does not parse (int: whole string; double: strtod) gives the default's number.
// cfg_bool: "0" is false, anything else true.
int cfg_int(const char *name);
double cfg_double(const char *name);
int cfg_bool(const char *name);

// ---- quality presets (cfg.c "quality presets", the tiers and their values in preset_pick.h). A tier is 0 low, 1 medium,
// 2 high (the game as shipped: sets nothing), 3 ultra. Its values are defaults: a key the player sets wins, then the
// device profile, then the preset in use, then the row's default. The preset key holds the player's choice
// (recommended, low, medium, high or ultra); cfg_start resolves it once per start. All of these: any thread once
// cfg_start has returned.
// "low", "medium", "high", "ultra"; "recommended" for any other number.
const char *cfg_preset_name(int tier);
// The tier whose values are in effect now; -1 before cfg_start.
int cfg_preset_in_use(void);
// The tier this PC gets (preset_pick.h preset_pick) and, in *why when not NULL, why in a few English words.
int cfg_preset_recommended(const char **why);
// 1 when a key the preset in use controls is set by the player to a value other than the one the preset gives it (the
// device profile's or the row's default where the tier sets none): the menu shows the preset as custom.
int cfg_preset_custom(void);
// The value tier gives key (prefix and case ignored) on this PC; NULL when the tier leaves it alone.
const char *cfg_preset_value(int tier, const char *key);
// Every key some tier sets on some hardware (upper case, no prefix), *n of them; the array lives for the process.
const char *const *cfg_preset_keys(int *n);
// 1 when the player's tier was chosen on other hardware and this run uses the recommended tier instead; it stays so
// at every start until the player picks a tier again (cfg_preset_chosen).
int cfg_preset_hw_changed(void);
// The player picked a tier in the menu: records this PC as the one it was chosen on (the state file preset-state.txt
// beside the game; never nx2saria.ini) and clears cfg_preset_hw_changed. The menu writes the preset key itself.
void cfg_preset_chosen(void);
// Puts tier's values in effect now (the menu's switch; the restart-only ones wait for a restart, cfg_pending), bumps
// cfg_version and logs each setting whose value changes ("preset: <old> -> <new>: <KEY> <before> -> <after>"). Keys
// the player set keep their values: the menu resets those it means to hand to the preset.
void cfg_preset_use(int tier);
