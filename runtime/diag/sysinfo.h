#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int sysinfo_text(char *buf, size_t n);
void sysinfo_cpu_name(char *out, size_t n);
int sysinfo_log_folder(char *out, size_t n);

#ifdef __cplusplus
}
#endif
