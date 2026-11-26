#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_LINE_CUT " [line cut]"

typedef struct {
    char *p;
    size_t cap, n;
    int cut;
} LogLine;

static inline void log_line_vadd(LogLine *l, const char *fmt, va_list ap) {
    if (l->cut) return;
    size_t room = l->cap - 1, left = room - l->n;
    int k = vsnprintf(l->p + l->n, left + 1, fmt, ap);
    if (k < 0) return;
    if ((size_t)k <= left) l->n += (size_t)k;
    else l->n = room, l->cut = 1;
}

static inline void log_line_add(LogLine *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void log_line_add(LogLine *l, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_line_vadd(l, fmt, ap);
    va_end(ap);
}

static inline size_t log_line_end(LogLine *l) {
    size_t mark = sizeof LOG_LINE_CUT - 1, room = l->cap - 1;
    if (l->cut) {
        memcpy(l->p + room - mark, LOG_LINE_CUT, mark);
        l->n = room;
    }
    l->p[l->n++] = '\n';
    size_t n = l->n;
    l->n = 0, l->cut = 0;
    return n;
}
