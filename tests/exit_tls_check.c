#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "logq.h"
#include "platform/platform.h"

void rt_log(const char *fmt, ...) { (void)fmt; }

static PlatAppend *file;
static void sink(const char *p, size_t n) { plat_append_write(file, p, n); }

__attribute__((noinline)) static void end_here(void) {
    ExitProcess(0);
}

int main(int argc, char **argv) {
    if (argc < 3 || !(file = plat_append_open(argv[2]))) return 2;
    logq_init(sink);
    if (!logq_async()) return 3;
    plat_exit_hook = logq_sync;
    plat_exit_note_setup(file, logq_peek);
    plat_exit_note_modules();
    logq_write("queued line\n", 12);
    const char *m = argv[1];
    if (!strcmp(m, "exitprocess")) end_here();
    if (!strcmp(m, "exit")) exit(0);
    if (!strcmp(m, "terminate")) TerminateProcess(GetCurrentProcess(), 0);
    if (!strcmp(m, "platexit")) plat_exit(0);
    if (!strcmp(m, "return")) {
        plat_exit_orderly();
        return 0;
    }
    return 2;
}
