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
