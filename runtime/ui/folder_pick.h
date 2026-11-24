#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*FolderPickDone)(const char *path, void *ctx);

void folder_pick(const char *start, FolderPickDone done, void *ctx);
void file_pick(const char *start, const char *ext, FolderPickDone done, void *ctx);
int folder_downloads(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
