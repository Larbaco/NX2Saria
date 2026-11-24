#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SAVE_SHARE_OK,
    SAVE_SHARE_NO_SAVE,
    SAVE_SHARE_READ_FAILED,
    SAVE_SHARE_WRITE_FAILED,
    SAVE_SHARE_NOT_A_SAVE,
    SAVE_SHARE_OTHER_GAME,
    SAVE_SHARE_NEWER,
    SAVE_SHARE_DAMAGED,
    SAVE_SHARE_UNSAFE,
} SaveShareResult;

SaveShareResult save_share_export(const char *dir, int bug_report, char *path, size_t cap);
SaveShareResult save_share_import(const char *zip_path, char *detail, size_t cap);
int save_share_pending(void);
int save_share_cancel(void);
int save_share_default_dir(char *out, size_t cap);
int save_share_backup_dir(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
