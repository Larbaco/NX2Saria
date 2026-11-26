#include <stdio.h>
#include <string.h>

#include "log_line.h"

static int failures;

static void expect(const char *what, const char *got, size_t n, const char *want) {
    if (n == strlen(want) && !memcmp(got, want, n)) return;
    failures++;
    printf("log_line_check: FAIL: %s: got '%.*s' (%zu bytes), expected '%s'\n", what, (int)n, got, n, want);
}

int main(void) {
    char buf[32];
    LogLine l = {buf, sizeof buf, 0, 0};

    log_line_add(&l, "FATAL: %s (%#x)", "x", 0xb);
    log_line_add(&l, ", reading");
    expect("pieces make one line", buf, log_line_end(&l), "FATAL: x (0xb), reading\n");

    expect("an empty line", buf, log_line_end(&l), "\n");

    log_line_add(&l, "%s", "0123456789012345678901234567890");
    expect("a line that fills the buffer exactly", buf, log_line_end(&l), "0123456789012345678901234567890\n");

    log_line_add(&l, "%s", "0123456789012345678901234567890");
    log_line_add(&l, "%s", "a");
    expect("one byte more", buf, log_line_end(&l), "01234567890123456789 [line cut]\n");

    log_line_add(&l, "FATAL: %s", "a text much longer than the buffer that holds it");
    log_line_add(&l, ", and more");
    expect("a long line", buf, log_line_end(&l), "FATAL: a text much l [line cut]\n");

    log_line_add(&l, "after a cut");
    expect("the next line starts clean", buf, log_line_end(&l), "after a cut\n");

    if (!failures) printf("log_line_check: passed (pieces in one line, an exact fit, a cut line marked)\n");
    return failures ? 1 : 0;
}
