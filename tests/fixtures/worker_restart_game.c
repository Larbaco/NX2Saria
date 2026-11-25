#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

static int start_copy(void) {
    wchar_t path[32768];
    wchar_t args[] = L"synthetic-game restarted";
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof startup;
    if (!GetModuleFileNameW(NULL, path, sizeof path / sizeof *path))
        return 1;
    if (!CreateProcessW(path, args, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process))
        return 2;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

static void pause_poll(void) {
    Sleep(50);
}
#else
#include <time.h>
#include <unistd.h>

static int start_copy(const char *executable) {
    pid_t child = fork();
    if (child < 0)
        return 1;
    if (child == 0) {
        execl(executable, executable, "restarted", (char *)NULL);
        _exit(2);
    }
    return 0;
}

static void pause_poll(void) {
    struct timespec delay = {0, 50000000};
    nanosleep(&delay, NULL);
}
#endif

int main(int argc, char **argv) {
    if (argc == 1) {
#ifdef _WIN32
        return start_copy();
#else
        return start_copy(argv[0]);
#endif
    }
    FILE *marker = fopen("restarted", "w");
    if (!marker)
        return 3;
    fclose(marker);
    for (int attempt = 0; attempt < 500; attempt++) {
        FILE *release = fopen("exit-restarted", "r");
        if (release) {
            fclose(release);
            return 7;
        }
        pause_poll();
    }
    return 4;
}
