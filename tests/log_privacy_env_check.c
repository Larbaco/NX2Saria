// tests/log_privacy_check.py builds this with runtime/log_privacy.c: the folders read from this process's environment
// (HOME, its resolved form, USER), the switch, the start line and the strings and copies the game writes. Stand-ins for
// the log, the settings and the environment. Usage: log_privacy_env_check <text>...: prints "log: <line>" for each log
// line and "str: <text short>", "dup: <text short or (none)>" for each argument, then "count: <n>".
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log_privacy.h"

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("log: ");
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

int cfg_bool(const char *name) {
    const char *v = getenv(name);
    return !(v && !strcmp(v, "0"));
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}

int main(int argc, char **argv) {
    log_privacy_init();
    log_privacy_start();
    for (int i = 1; i < argc; i++) {
        char buf[1024];
        printf("str: %s\n", log_privacy_str(argv[i], buf, sizeof buf));
        size_t n = 0;
        char *d = log_privacy_dup(argv[i], strlen(argv[i]), &n);
        printf("dup: %.*s\n", d ? (int)n : 6, d ? d : "(none)");
        free(d);
    }
    printf("count: %llu\n", log_privacy_count());
    return 0;
}
