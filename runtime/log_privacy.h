#pragma once
#include <stddef.h>

void log_privacy_init(void);
void log_privacy_start(void);
int log_privacy_on(void);

size_t log_privacy_find(const char *p, size_t n);
size_t log_privacy_rewrite(const char *p, size_t n, size_t *at, char *out, size_t cap);
const char *log_privacy_str(const char *s, char *out, size_t cap);
const char *log_privacy_str_always(const char *s, char *out, size_t cap);
char *log_privacy_dup(const char *p, size_t n, size_t *out_n);
unsigned long long log_privacy_count(void);
