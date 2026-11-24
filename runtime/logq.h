#pragma once
#include <stddef.h>

typedef void (*LogqSink)(const char *p, size_t n);

void logq_init(LogqSink sink);
int logq_async(void);
void logq_write(const char *p, size_t n);
void logq_sync(void);
void logq_flush(void);
void logq_nudge(void);
void logq_peek(const char **p, size_t *n);
size_t logq_bytes(void);
