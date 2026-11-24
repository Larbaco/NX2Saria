#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "platform/platform.h"

void rt_log(const char *fmt, ...) { (void)fmt; }

static void out(const char *s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &w, NULL);
}

__attribute__((noinline)) static void log2k(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out(buf);
}

static const char *file;

static void crash(const PlatCrash *x) {
    static const char text[] = "the process faulted while the boot built pipelines\n";
    int wrote = plat_file_write(file, text, sizeof text - 1);
    log2k("report line 1: %s (%#x) on thread %s, the file written: %d\n", x->what, x->code,
          plat_thread_name() ? plat_thread_name() : "(unnamed)", wrote);
    log2k("report line 2: %d host frames\n", x->nframes);
    void *f[62];
    USHORT n = RtlCaptureStackBackTrace(0, 62, f, NULL);
    log2k("report line 3: %u frames captured; the end\n", (unsigned)n);
    TerminateProcess(GetCurrentProcess(), 71);
}

static volatile int never;
__attribute__((noinline)) static int recurse(int d) {
    if (never) return 0;
    volatile char pad[1024];
    pad[0] = (char)d;
    return recurse(d + 1) + pad[0];
}

static void overflower(void *arg) {
    (void)arg;
    recurse(0);
}

static PlatFiber *main_fiber;
static void fiber_body(void *arg) {
    (void)arg;
    recurse(0);
    plat_fiber_switch(main_fiber);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    file = argv[2];
    plat_crash_hook(crash);
    if (!strcmp(argv[1], "thread")) {
        plat_thread_spawn(overflower, NULL, "deep", 0);
    } else {
        main_fiber = plat_fiber_self();
        PlatFiber *f = plat_fiber_new(1 << 20, fiber_body, NULL);
        if (!main_fiber || !f) return 3;
        plat_fiber_switch(f);
    }
    Sleep(10000);
    out("main: no crash ended the process in 10 s\n");
    return 0;
}
