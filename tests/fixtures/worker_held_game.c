#include <windows.h>
#include <stdio.h>

int main(void) {
    FILE *pid = fopen("game-pid", "w");
    if (!pid) return 1;
    fprintf(pid, "%lu", (unsigned long)GetCurrentProcessId());
    fclose(pid);
    for (int i = 0; i < 600; i++) {
        if (GetFileAttributesA("finish-game") != INVALID_FILE_ATTRIBUTES) return 0;
        Sleep(100);
    }
    return 2;
}
