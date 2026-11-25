#pragma once
#include <stddef.h>

const char *cfg_env(const char *name);

const char *cfg_get(const char *name);

void cfg_init(void);
void cfg_start(int game);

int cfg_load(const char *path);

typedef struct { int changed, restart, rejected, env, kept; } CfgReloadResult;
int cfg_reload_file(CfgReloadResult *r);

void cfg_set(const char *name, const char *value);
unsigned cfg_version(void);
int cfg_setenv(const char *name, const char *value);
enum { CFG_DIRTY = 1, CFG_REMOVED = 2, CFG_TEST = 4 };
int cfg_state(const char *name, const char **value);
void cfg_hold(const char *name, int on);
void cfg_revert(const char *name, const char *orig_value, int orig_state);
void cfg_set_test(const char *name, const char *value);
int cfg_test_keep(void);
int cfg_from_env(const char *name);
int cfg_is_set(const char *name);
const char *cfg_unknown_keys(int *count);
const char *cfg_device_profile(void);
int cfg_save(void);
int cfg_save_to(const char *path);

int cfg_reset(const char *const *keys, int n);
int cfg_last_save_ok(void);
void cfg_each_active(void (*cb)(const char *key, const char *value, int from_env, void *ctx), void *ctx);

typedef struct { const char *key, *def, *values; int live; } CfgMeta;
int cfg_supported(const char *key);
int cfg_has_section(const char *key);
void cfg_log_unsupported(void);
const CfgMeta *cfg_meta(const char *key);
const CfgMeta *cfg_meta_at(int i);
const char *cfg_default(const char *key);
int cfg_pending(const char *key);
int cfg_pending_count(void);
int cfg_pending_for(const char *key, const char *value);

const char *cfg_value(const char *name);
int cfg_int(const char *name);
double cfg_double(const char *name);
int cfg_bool(const char *name);

const char *cfg_preset_name(int tier);
int cfg_preset_in_use(void);
int cfg_preset_recommended(const char **why);
int cfg_preset_custom(void);
const char *cfg_preset_value(int tier, const char *key);
const char *const *cfg_preset_keys(int *n);
int cfg_preset_hw_changed(void);
void cfg_preset_chosen(void);
void cfg_preset_use(int tier);
void cfg_preset_use_from(int tier, const char *const *from_values);
const char *cfg_default_tier(const char *key, int tier);
int cfg_preset_differs(int tier, const char *key, const char *value);
int cfg_preset_notice_due(void);
void cfg_preset_notice_done(void);
void cfg_preset_device_started(void);
