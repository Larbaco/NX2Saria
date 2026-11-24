// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define FS_R_PATH_NOT_FOUND 0x202u
#define FS_R_ALREADY_EXISTS 0x402u
#define FS_R_NOT_EMPTY 0x1002u
#define FS_R_UNSUPPORTED_OPEN 0x6402u
#define FS_R_INVALID 0xe402u
#define FS_R_TARGET_NOT_FOUND 0x7d402u
#define FS_R_INVALID_ARGUMENT 0x2ee202u
#define FS_R_INVALID_PATH 0x2ee602u
#define FS_R_OPEN_MODE 0x307402u
#define FS_R_APPEND_REQUIRED 0x2f5e02u

enum { FS_OPEN_READ = 1, FS_OPEN_WRITE = 2, FS_OPEN_APPEND = 4 };
enum { FS_ENTRY_DIRECTORY = 0, FS_ENTRY_FILE = 1 };

#define ROMFS_HANDLE_BASE 0x100000000ull
int romfs_owns(const char *path);
int romfs_handle(uint64_t h);
uint32_t romfs_mount(const char *name);
uint32_t romfs_open(uint64_t *out, const char *path, int mode);
uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
uint32_t romfs_size(int64_t *out, uint64_t h);
void romfs_close(uint64_t h);
uint8_t *romfs_load(const char *rel, uint64_t *size_out);
uint32_t romfs_entry_type(int *out, const char *path);
uint32_t romfs_list(const char *path, int (*fn)(const char *name, int is_dir, uint64_t size, void *ctx), void *ctx);
uint64_t romfs_file_count(void);
const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size);
const char *romfs_image_path(void);
size_t romfs_overlay_count(void);
int romfs_patches_loaded(void);
enum { ROMFS_PROMPTS_OK, ROMFS_PROMPTS_MISSING, ROMFS_PROMPTS_FAILED, ROMFS_PROMPTS_SHAPE, ROMFS_PROMPTS_MODS,
       ROMFS_PROMPTS_PATCHES };
const char *romfs_prompts(void);
int romfs_prompts_ready(const char *folder);
int romfs_prompts_off(const char *folder);
int romfs_action_prompt(void);
const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added);
int romfs_read_patch(const char *rel, uint64_t offset, const void *orig, const void *repl, uint32_t n, char *why, size_t cap);
void romfs_read_patches_off(void);
void fs_trace(const char *what, const char *path);

void fs_host_set_profile(const char *dir);
void fs_host_lock_profile(void);
const char *fs_save_dir(void);
int fs_save_busy(void);
void fs_save_flush(void);
void fs_save_wait(void);
typedef int (*FsSaveVisit)(void *ctx, const char *rel, const uint8_t *data, size_t n);
uint8_t *fs_save_load(const char *rel, uint64_t *size);
int fs_save_export(FsSaveVisit visit, void *ctx);
int fs_save_import_stage(int count, const char *const *rel, const uint8_t *const *data, const size_t *size);
int fs_save_import_waiting(void);
int fs_save_import_cancel(void);
const char *fs_save_import_done(void);
int fs_save_snapshot(const char *folder, const char *prefix, int keep, char *out, size_t cap);
