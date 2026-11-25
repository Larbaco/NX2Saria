#include <stdio.h>
#include <windows.h>

#include "host.h"
#include "platform/platform.h"

static void say(const char *s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &w, NULL);
}

static void NTAPI tls_cb(PVOID h, DWORD reason, PVOID reserved) {
    (void)h, (void)reserved;
    if (reason == DLL_PROCESS_DETACH) say("detach: the libraries' detach calls ran\n");
}
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK p_tls_cb = tls_cb;
__thread int tls_var;
extern const IMAGE_TLS_DIRECTORY _tls_used;
const void *volatile keep_tls = &_tls_used;

void rt_log_ending(void) {}
void rt_log_sync(void) {}
void rt_log_raw(const char *p, size_t n) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), p, (DWORD)n, &w, NULL);
}
void rt_vlogf(const char *fmt, va_list ap) {
    char b[1024];
    int n = vsnprintf(b, sizeof b, fmt, ap);
    if (n > 0) rt_log_raw(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}
void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
    rt_log_raw("\n", 1);
}
void kern_free_cores_note(void) {}
void prof_sampler_write(void) {}
void rtss_limit_crash(void) { say("rtss: left to the next start\n"); }
void kern_checkpoint(void) {}

void perf_summary_flush_try(void) { say("frametime: at a fatal error\n"); }

static int rumble_hangs;
void input_rumble_hd_stop(void) {
    if (rumble_hangs) Sleep(INFINITE);
    say("rumble: stopped\n");
}

static void crash(const PlatCrash *x) {
    if (!rt_ending_enter(71)) plat_exit(71);
    char b[160];
    for (int i = 1; i <= 3; i++) {
        snprintf(b, sizeof b, "report line %d: %s on thread %lu\n", i, x->what, (unsigned long)GetCurrentThreadId());
        say(b);
        Sleep(100);
    }
    plat_exit(71);
}

static volatile LONG go;
static DWORD WINAPI fault(void *arg) {
    Sleep((DWORD)(uintptr_t)arg);
    while (!go) Sleep(0);
    *(volatile int *)(uintptr_t)16 = 1;
    return 0;
}
static DWORD WINAPI fatal_thread(void *arg) {
    Sleep((DWORD)(uintptr_t)arg);
    rt_fatal("the test's fatal error %d", 2);
}
static void rtss_thread(void *arg) {
    (void)arg;
    rt_fatal("the test's fatal error %d", 3);
}
static void slow_note(void) {
    say("fatal note: start\n");
    Sleep(300);
    say("fatal note: end\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "";
    tls_var = 1;
    plat_crash_hook(crash);
    if (!strcmp(mode, "fatal")) rt_fatal("the test's fatal error %d", 1);
    if (!strcmp(mode, "fatal_rumble")) {
        rumble_hangs = 1;
        rt_fatal("the test's fatal error %d", 1);
    }
    if (!strcmp(mode, "crash_crash")) {
        CloseHandle(CreateThread(NULL, 0, fault, (void *)0, 0, NULL));
        CloseHandle(CreateThread(NULL, 0, fault, (void *)0, 0, NULL));
        Sleep(50);
        go = 1;
        Sleep(10000);
        return 3;
    }
    if (!strcmp(mode, "fatal_crash")) {
        rt_fatal_note_add(slow_note);
        go = 1;
        CloseHandle(CreateThread(NULL, 0, fault, (void *)100, 0, NULL));
        rt_fatal("the test's fatal error %d", 1);
    }
    if (!strcmp(mode, "crash_fatal")) {
        CloseHandle(CreateThread(NULL, 0, fatal_thread, (void *)100, 0, NULL));
        go = 1;
        fault(0);
        return 3;
    }
    if (!strcmp(mode, "fatal_rtss")) {
        plat_thread_spawn(rtss_thread, NULL, "rtss", 0);
        Sleep(10000);
        return 3;
    }
    printf("usage: fatal_exit_check fatal|crash_crash|fatal_crash|crash_fatal|fatal_rumble|fatal_rtss\n");
    return 2;
}
